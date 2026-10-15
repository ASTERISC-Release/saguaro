/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/*
 * Implementation of the extended-attribute syscalls: `setxattr`, `getxattr`, `listxattr` and
 * `removexattr` with their `l` (no symlink follow) and `f` (descriptor) variants.
 *
 * The LibOS has nowhere to store attributes: the emulated tmpfs keeps only the POSIX metadata, and
 * the PAL has no extended-attribute operation (a host-backed file's attributes would need one, plus
 * a virtio-fs client call for the VM PAL). So a call that reaches a file reports `EOPNOTSUPP` — the
 * normal answer on a filesystem without extended attributes (ext2 without the feature, squashfs,
 * NFSv2, ...), which applications handle by not using them — after performing the same argument and
 * path validation the kernel does first. Storage is tracked in `TODO.md` for when an application
 * needs it.
 */

#include "libos_fs.h"
#include "libos_handle.h"
#include "libos_internal.h"
#include "libos_table.h"
#include "libos_thread.h"
#include "libos_utils.h"
#include <linux/fs.h> /* `AT_*` (through the headers) */

/* from the kernel's uapi `linux/xattr.h`, which is not in the LibOS include chain */
#define XATTR_NAME_MAX 255
#define XATTR_SIZE_MAX 65536

/*
 * The kernel order we mirror: validate the buffers, resolve the path (following symlinks unless
 * this is an `l` variant), and only then report that this filesystem has no attributes. A nonempty
 * value's buffer is validated too (`setxattr` with a bad address and a nonzero size fails with
 * `EFAULT`).
 */
static long xattr_path(const char* pathname, const char* name, const void* value, size_t size,
                       bool follow) {
    if (!is_user_string_readable(pathname) || !is_user_string_readable(name))
        return -EFAULT;
    if (size > XATTR_SIZE_MAX)
        return -E2BIG;
    if (strlen(name) > XATTR_NAME_MAX)
        return -ERANGE;
    if (value && size > 0 && !is_user_memory_readable((void*)value, size))
        return -EFAULT;

    struct libos_dentry* dir = NULL;
    struct libos_dentry* dent = NULL;
    if (*pathname != '/' && get_dirfd_dentry(AT_FDCWD, &dir) < 0)
        return -EBADF;

    lock(&g_dcache_lock);
    int ret = path_lookupat(dir, pathname, follow ? LOOKUP_FOLLOW : LOOKUP_NO_FOLLOW, &dent);
    unlock(&g_dcache_lock);
    if (dir)
        put_dentry(dir);
    if (dent)
        put_dentry(dent);
    if (ret < 0)
        return ret;
    return -EOPNOTSUPP; /* the file exists; this filesystem just has no attributes (see above) */
}

/* `value == NULL` marks the get/list flavors, whose buffer is writable and may have size 0 (the
 * kernel's size query). */
static long xattr_get_path(const char* pathname, const char* name, void* value, size_t size,
                           bool follow) {
    if (!is_user_string_readable(pathname) || !is_user_string_readable(name))
        return -EFAULT;
    if (size > XATTR_SIZE_MAX)
        return -ERANGE; /* `getxattr` limits the buffer with `ERANGE`, not `E2BIG` */
    if (strlen(name) > XATTR_NAME_MAX)
        return -ERANGE;
    if (value && !is_user_memory_writable(value, size))
        return -EFAULT;

    struct libos_dentry* dir = NULL;
    struct libos_dentry* dent = NULL;
    if (*pathname != '/' && get_dirfd_dentry(AT_FDCWD, &dir) < 0)
        return -EBADF;

    lock(&g_dcache_lock);
    int ret = path_lookupat(dir, pathname, follow ? LOOKUP_FOLLOW : LOOKUP_NO_FOLLOW, &dent);
    unlock(&g_dcache_lock);
    if (dir)
        put_dentry(dir);
    if (dent)
        put_dentry(dent);
    if (ret < 0)
        return ret;
    return -EOPNOTSUPP;
}

/* the descriptor variants check the descriptor first (the `EBADF` comes before anything else) */
static long xattr_fd(int fd, const char* name, const void* value, size_t size, bool is_get) {
    struct libos_handle* hdl = get_fd_handle(fd, NULL, NULL);
    if (!hdl)
        return -EBADF;
    put_handle(hdl);

    if (!is_user_string_readable(name))
        return -EFAULT;
    if (is_get) {
        if (size > XATTR_SIZE_MAX) {
            return -ERANGE;
        }
    } else {
        if (size > XATTR_SIZE_MAX) {
            return -E2BIG;
        }
    }
    if (strlen(name) > XATTR_NAME_MAX)
        return -ERANGE;
    if (is_get) {
        if (value && !is_user_memory_writable((void*)value, size))
            return -EFAULT;
    } else if (value && size > 0 && !is_user_memory_readable((void*)value, size)) {
        return -EFAULT;
    }
    return -EOPNOTSUPP;
}

long libos_syscall_setxattr(const char* path, const char* name, const void* value, size_t size,
                            int flags) {
    (void)flags; /* XATTR_CREATE/XATTR_REPLACE only matter once attributes can exist */
    return xattr_path(path, name, value, size, /*follow=*/true);
}

long libos_syscall_lsetxattr(const char* path, const char* name, const void* value, size_t size,
                             int flags) {
    (void)flags;
    return xattr_path(path, name, value, size, /*follow=*/false);
}

long libos_syscall_fsetxattr(int fd, const char* name, const void* value, size_t size, int flags) {
    (void)flags;
    return xattr_fd(fd, name, value, size, /*is_get=*/false);
}

long libos_syscall_getxattr(const char* path, const char* name, void* value, size_t size) {
    return xattr_get_path(path, name, value, size, /*follow=*/true);
}

long libos_syscall_lgetxattr(const char* path, const char* name, void* value, size_t size) {
    return xattr_get_path(path, name, value, size, /*follow=*/false);
}

long libos_syscall_fgetxattr(int fd, const char* name, void* value, size_t size) {
    return xattr_fd(fd, name, value, size, /*is_get=*/true);
}

long libos_syscall_listxattr(const char* path, char* list, size_t size) {
    return xattr_get_path(path, /*name=*/"", list, size, /*follow=*/true);
}

long libos_syscall_llistxattr(const char* path, char* list, size_t size) {
    return xattr_get_path(path, /*name=*/"", list, size, /*follow=*/false);
}

long libos_syscall_flistxattr(int fd, char* list, size_t size) {
    return xattr_fd(fd, /*name=*/"", list, size, /*is_get=*/true);
}

long libos_syscall_removexattr(const char* path, const char* name) {
    return xattr_path(path, name, /*value=*/NULL, /*size=*/0, /*follow=*/true);
}

long libos_syscall_lremovexattr(const char* path, const char* name) {
    return xattr_path(path, name, /*value=*/NULL, /*size=*/0, /*follow=*/false);
}

long libos_syscall_fremovexattr(int fd, const char* name) {
    return xattr_fd(fd, name, /*value=*/NULL, /*size=*/0, /*is_get=*/false);
}
