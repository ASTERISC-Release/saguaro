/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/*
 * Implementation of the extended-attribute syscalls: `setxattr`, `getxattr`, `listxattr` and
 * `removexattr` with their `l` (no symlink follow) and `f` (descriptor) variants.
 *
 * The syscall layer performs the kernel's checks (buffers, name length, size limits, flags, the
 * namespace rules of `xattr_permission()`) and hands the operation to the filesystem's `xattr`
 * hook: host-backed files (chroot) store their attributes on the host through `PalFileXattr()`
 * (the host filesystem decides what it supports), tmpfs keeps them in the inode
 * (`libos_xattr_store_op()`), and a filesystem without the hook answers EOPNOTSUPP, the normal
 * answer of a filesystem without extended attributes.
 */

#include "libos_fs.h"
#include "libos_handle.h"
#include "libos_internal.h"
#include "libos_lock.h"
#include "libos_table.h"
#include "libos_thread.h"
#include "libos_utils.h"
#include "pal.h"
#include "stat.h"
#include <linux/fs.h> /* `AT_*` (through the headers) */

/* from the kernel's uapi `linux/xattr.h`, which is not in the LibOS include chain */
#define XATTR_NAME_MAX 255
#define XATTR_SIZE_MAX 65536
#define XATTR_LIST_MAX 65536
#define XATTR_CREATE   0x1
#define XATTR_REPLACE  0x2

#define LIBOS_CAP_SYS_ADMIN 21

/* ---- the in-inode store (tmpfs) ---- */

struct libos_xattr_entry {
    struct libos_xattr_entry* next;
    size_t size;
    char* name;
    char value[];
};

void libos_xattr_store_free(struct libos_inode* inode) {
    struct libos_xattr_entry* e = inode->xattrs;
    while (e) {
        struct libos_xattr_entry* next = e->next;
        free(e->name);
        free(e);
        e = next;
    }
    inode->xattrs = NULL;
}

int libos_xattr_store_op(struct libos_inode* inode, int op, const char* name, void* value,
                         size_t* size, int flags) {
    int ret = 0;
    lock(&inode->lock);
    struct libos_xattr_entry** pp = &inode->xattrs;
    if (op != PAL_XATTR_LIST) {
        while (*pp && strcmp((*pp)->name, name))
            pp = &(*pp)->next;
    }
    struct libos_xattr_entry* e = *pp;

    switch (op) {
        case PAL_XATTR_GET:
            if (!e) {
                ret = -ENODATA;
            } else if (*size && *size < e->size) {
                ret = -ERANGE;
            } else {
                if (*size)
                    memcpy(value, e->value, e->size);
                *size = e->size;
            }
            break;
        case PAL_XATTR_LIST: {
            size_t total = 0;
            for (struct libos_xattr_entry* it = inode->xattrs; it; it = it->next)
                total += strlen(it->name) + 1;
            if (*size && *size < total) {
                ret = -ERANGE;
                break;
            }
            if (*size) {
                char* out = value;
                for (struct libos_xattr_entry* it = inode->xattrs; it; it = it->next) {
                    size_t len = strlen(it->name) + 1;
                    memcpy(out, it->name, len);
                    out += len;
                }
            }
            *size = total;
            break;
        }
        case PAL_XATTR_SET: {
            if ((flags & PAL_XATTR_CREATE) && e) {
                ret = -EEXIST;
                break;
            }
            if ((flags & PAL_XATTR_REPLACE) && !e) {
                ret = -ENODATA;
                break;
            }
            struct libos_xattr_entry* fresh = malloc(sizeof(*fresh) + *size);
            char* fresh_name = strdup(name);
            if (!fresh || !fresh_name) {
                free(fresh);
                free(fresh_name);
                ret = -ENOMEM;
                break;
            }
            fresh->name = fresh_name;
            fresh->size = *size;
            if (*size)
                memcpy(fresh->value, value, *size);
            if (e) {
                fresh->next = e->next;
                free(e->name);
                free(e);
            } else {
                fresh->next = NULL;
            }
            *pp = fresh;
            break;
        }
        case PAL_XATTR_REMOVE:
            if (!e) {
                ret = -ENODATA;
            } else {
                *pp = e->next;
                free(e->name);
                free(e);
            }
            break;
        default:
            ret = -EINVAL;
    }
    unlock(&inode->lock);
    return ret;
}

/* ---- the syscalls ---- */

enum xattr_ns { NS_USER, NS_TRUSTED, NS_SECURITY, NS_SYSTEM, NS_UNKNOWN };

static enum xattr_ns name_namespace(const char* name) {
    if (!strncmp(name, "user.", 5))
        return NS_USER;
    if (!strncmp(name, "trusted.", 8))
        return NS_TRUSTED;
    if (!strncmp(name, "security.", 9))
        return NS_SECURITY;
    if (!strncmp(name, "system.", 7))
        return NS_SYSTEM;
    return NS_UNKNOWN;
}

/* The kernel's `xattr_permission()` for an attribute operation on `dent` (the caller holds
 * `g_dcache_lock`): unknown namespaces are EOPNOTSUPP, `trusted.` needs CAP_SYS_ADMIN (reads then
 * see ENODATA), `user.` attributes exist only on regular files and directories and need read or
 * write permission on the file. */
static int xattr_permission(struct libos_dentry* dent, const char* name, bool write) {
    enum xattr_ns ns = name_namespace(name);
    switch (ns) {
        case NS_UNKNOWN:
            return -EOPNOTSUPP;
        case NS_TRUSTED:
            if (!libos_capable(LIBOS_CAP_SYS_ADMIN))
                return write ? -EPERM : -ENODATA;
            return 0;
        case NS_USER:
            if (dent->inode->type != S_IFREG && dent->inode->type != S_IFDIR)
                return write ? -EPERM : -ENODATA;
            return check_permissions(dent, write ? MAY_WRITE : MAY_READ);
        default:
            return 0;
    }
}

/* The operation on a resolved dentry; `value`/`size` are the user's buffer and size. */
static long xattr_on_dentry(struct libos_dentry* dent, int op, const char* name, void* value,
                            size_t size, int flags) {
    assert(locked(&g_dcache_lock));
    if (!dent->inode)
        return -ENOENT;
    if (op != PAL_XATTR_LIST) {
        int ret = xattr_permission(dent, name,
                                   /*write=*/op == PAL_XATTR_SET || op == PAL_XATTR_REMOVE);
        if (ret < 0)
            return ret;
    }
    struct libos_fs* fs = dent->inode->fs;
    if (!fs || !fs->d_ops || !fs->d_ops->xattr)
        return -EOPNOTSUPP;

    /* GET and LIST go through a LibOS buffer so the filesystem never writes user memory */
    void* buf = NULL;
    if ((op == PAL_XATTR_GET || op == PAL_XATTR_LIST) && size) {
        buf = malloc(size);
        if (!buf)
            return -ENOMEM;
    }
    size_t io_size = size;
    long ret = fs->d_ops->xattr(dent, op, name, buf ? buf : value, &io_size,
                                flags & (PAL_XATTR_CREATE | PAL_XATTR_REPLACE));
    if (ret == 0 && (op == PAL_XATTR_GET || op == PAL_XATTR_LIST)) {
        if (op == PAL_XATTR_LIST && !libos_capable(LIBOS_CAP_SYS_ADMIN) && size) {
            /* `trusted.` names are listed only for CAP_SYS_ADMIN */
            char* in = buf;
            char* out = buf;
            char* end = (char*)buf + io_size;
            while (in < end) {
                size_t len = strnlen(in, end - in) + 1;
                if (name_namespace(in) != NS_TRUSTED) {
                    memmove(out, in, len);
                    out += len;
                }
                in += len;
            }
            io_size = out - (char*)buf;
        }
        if (size)
            memcpy(value, buf, io_size);
        ret = (long)io_size;
    }
    free(buf);
    return ret;
}

static long check_name(const char* name) {
    if (!is_user_string_readable(name))
        return -EFAULT;
    size_t len = strlen(name);
    if (len == 0 || len > XATTR_NAME_MAX)
        return -ERANGE;
    return 0;
}

/* Validation of the arguments, in the kernel's order, before the file is looked at. */
static long check_args(int op, const char* name, const void* value, size_t* size, int flags) {
    if (op == PAL_XATTR_SET && (flags & ~(XATTR_CREATE | XATTR_REPLACE)))
        return -EINVAL;
    if (op != PAL_XATTR_LIST) {
        long ret = check_name(name);
        if (ret < 0)
            return ret;
    }
    if (op == PAL_XATTR_SET) {
        if (*size > XATTR_SIZE_MAX)
            return -E2BIG;
        if (*size && !is_user_memory_readable(value, *size))
            return -EFAULT;
    } else if (op == PAL_XATTR_GET || op == PAL_XATTR_LIST) {
        size_t max = op == PAL_XATTR_LIST ? XATTR_LIST_MAX : XATTR_SIZE_MAX;
        if (*size > max)
            *size = max; /* the kernel clamps the buffer it uses */
        if (*size && !is_user_memory_writable((void*)value, *size))
            return -EFAULT;
    }
    return 0;
}

static long xattr_path(const char* pathname, int op, const char* name, void* value, size_t size,
                       int flags, bool follow) {
    if (!is_user_string_readable(pathname))
        return -EFAULT;
    long ret = check_args(op, name, value, &size, flags);
    if (ret < 0)
        return ret;

    struct libos_dentry* dir = NULL;
    struct libos_dentry* dent = NULL;
    if (*pathname != '/' && (ret = get_dirfd_dentry(AT_FDCWD, &dir)) < 0)
        return ret;

    lock(&g_dcache_lock);
    ret = path_lookupat(dir, pathname, follow ? LOOKUP_FOLLOW : LOOKUP_NO_FOLLOW, &dent);
    if (ret == 0)
        ret = xattr_on_dentry(dent, op, name, value, size, flags);
    unlock(&g_dcache_lock);
    if (dir)
        put_dentry(dir);
    if (dent)
        put_dentry(dent);
    return ret;
}

/* the descriptor variants check the descriptor first (the `EBADF` comes before anything else) */
static long xattr_fd(int fd, int op, const char* name, void* value, size_t size, int flags) {
    struct libos_handle* hdl = get_fd_handle(fd, NULL, NULL);
    if (!hdl)
        return -EBADF;
    long ret = check_args(op, name, value, &size, flags);
    if (ret == 0) {
        lock(&g_dcache_lock);
        ret = hdl->dentry ? xattr_on_dentry(hdl->dentry, op, name, value, size, flags)
                          : -EOPNOTSUPP; /* pipes and sockets have no attributes here */
        unlock(&g_dcache_lock);
    }
    put_handle(hdl);
    return ret;
}

long libos_syscall_setxattr(const char* path, const char* name, const void* value, size_t size,
                            int flags) {
    return xattr_path(path, PAL_XATTR_SET, name, (void*)value, size, flags, /*follow=*/true);
}

long libos_syscall_lsetxattr(const char* path, const char* name, const void* value, size_t size,
                             int flags) {
    return xattr_path(path, PAL_XATTR_SET, name, (void*)value, size, flags, /*follow=*/false);
}

long libos_syscall_fsetxattr(int fd, const char* name, const void* value, size_t size, int flags) {
    return xattr_fd(fd, PAL_XATTR_SET, name, (void*)value, size, flags);
}

long libos_syscall_getxattr(const char* path, const char* name, void* value, size_t size) {
    return xattr_path(path, PAL_XATTR_GET, name, value, size, 0, /*follow=*/true);
}

long libos_syscall_lgetxattr(const char* path, const char* name, void* value, size_t size) {
    return xattr_path(path, PAL_XATTR_GET, name, value, size, 0, /*follow=*/false);
}

long libos_syscall_fgetxattr(int fd, const char* name, void* value, size_t size) {
    return xattr_fd(fd, PAL_XATTR_GET, name, value, size, 0);
}

long libos_syscall_listxattr(const char* path, char* list, size_t size) {
    return xattr_path(path, PAL_XATTR_LIST, NULL, list, size, 0, /*follow=*/true);
}

long libos_syscall_llistxattr(const char* path, char* list, size_t size) {
    return xattr_path(path, PAL_XATTR_LIST, NULL, list, size, 0, /*follow=*/false);
}

long libos_syscall_flistxattr(int fd, char* list, size_t size) {
    return xattr_fd(fd, PAL_XATTR_LIST, NULL, list, size, 0);
}

long libos_syscall_removexattr(const char* path, const char* name) {
    return xattr_path(path, PAL_XATTR_REMOVE, name, NULL, 0, 0, /*follow=*/true);
}

long libos_syscall_lremovexattr(const char* path, const char* name) {
    return xattr_path(path, PAL_XATTR_REMOVE, name, NULL, 0, 0, /*follow=*/false);
}

long libos_syscall_fremovexattr(int fd, const char* name) {
    return xattr_fd(fd, PAL_XATTR_REMOVE, name, NULL, 0, 0);
}
