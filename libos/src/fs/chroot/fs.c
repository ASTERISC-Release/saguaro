/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2014 Stony Brook University
 * Copyright (C) 2021 Intel Corporation
 *                    Paweł Marczewski <pawel@invisiblethingslab.com>
 */

/*
 * This file contains code for implementation of 'chroot' filesystem.
 *
 * TODO: reintroduce the file position sync (using libos_fs_sync.h) after the migration to inodes is
 * finished.
 */

#include "libos_flags_conv.h"
#include "libos_fs.h"
#include "libos_handle.h"
#include "libos_internal.h"
#include "libos_lock.h"
#include "libos_utils.h"
#include "libos_vma.h"
#include "linux_abi/errors.h"
#include "linux_abi/fs.h"
#include "linux_abi/memory.h"
#include "pal.h"
#include "perm.h"
#include "stat.h"

#define UTIME_NOW  ((1L << 30) - 1)
#define UTIME_OMIT ((1L << 30) - 2)

/*
 * Always add a read permission to files created on host, because PAL requires opening the file even
 * for operations such as `unlink` or `chmod`.
 *
 * The updated file permissions will not be visible to the process creating the file or updating its
 * permissions, e.g. if a process creates a write-only file, Gramine's `stat` will still report it
 * as write-only. However, other Gramine processes accessing that file afterwards will see the
 * updated permissions.
 */
#define HOST_PERM(perm) ((perm) | PERM_r________)

static int chroot_mount(struct libos_mount_params* params, void** mount_data) {
    __UNUSED(mount_data);
    if (!params->uri || (!strstartswith(params->uri, URI_PREFIX_FILE) &&
                         !strstartswith(params->uri, URI_PREFIX_DEV)))
        return -EINVAL;
    return 0;
}

static int chroot_setup_dentry(struct libos_dentry* dent, mode_t type, mode_t perm,
                               file_off_t size) {
    assert(locked(&g_dcache_lock));
    assert(!dent->inode);

    struct libos_inode* inode = get_new_inode(dent->mount, type, perm);
    if (!inode)
        return -ENOMEM;
    inode->size = size;
    dent->inode = inode;
    return 0;
}

static int chroot_lookup(struct libos_dentry* dent) {
    assert(locked(&g_dcache_lock));

    int ret;

    /*
     * We don't know the file type yet, so we can't construct a PAL URI with the right prefix. In
     * most cases, a "file:" prefix is good enough: `PalStreamAttributesQuery` will access the file
     * and report the right file type.
     */
    char* uri = NULL;
    ret = dentry_uri(dent, S_IFREG, &uri);
    if (ret < 0)
        goto out;

    PAL_STREAM_ATTR pal_attr;
    memset(&pal_attr, 0, sizeof(pal_attr));
    ret = PalStreamAttributesQuery(uri, &pal_attr);
    if (ret < 0) {
        ret = pal_to_unix_errno(ret);
        goto out;
    }

    mode_t type;
    switch (pal_attr.handle_type) {
        case PAL_TYPE_FILE:
            type = S_IFREG;
            break;
        case PAL_TYPE_DIR:
            type = S_IFDIR;
            break;
        case PAL_TYPE_DEV:
            type = S_IFCHR;
            break;
        case PAL_TYPE_PIPE:
            log_warning("trying to access '%s' which is a host-level FIFO (named pipe); "
                        "Gramine supports only named pipes created by Gramine processes",
                        uri);
            ret = -EACCES;
            goto out;
        default:
            log_error("unexpected handle type returned by PAL: %d", pal_attr.handle_type);
            BUG();
    }

    mode_t perm = pal_attr.share_flags;

    file_off_t size = (type == S_IFREG ? pal_attr.pending_size : 0);

    ret = chroot_setup_dentry(dent, type, perm, size);
    if (ret == 0 && type == S_IFCHR) {
        /* zero unless the PAL reports it (only the Linux PAL does) */
        dent->inode->rdev_major = pal_attr.dev.rdev_major;
        dent->inode->rdev_minor = pal_attr.dev.rdev_minor;
    }
out:
    free(uri);
    return ret;
}

/* Open a temporary read-only PAL handle for a file (used by `unlink` etc.) */
static int chroot_temp_open(struct libos_dentry* dent, PAL_HANDLE* out_palhdl) {
    char* uri;
    mode_t uri_type = dent->inode->type == S_IFDIR ? S_IFDIR : S_IFREG;
    int ret = dentry_uri(dent, uri_type, &uri);
    if (ret < 0)
        return ret;

    ret = PalStreamOpen(uri, PAL_ACCESS_RDONLY, /*share_flags=*/0, PAL_CREATE_NEVER,
                        /*options=*/0, out_palhdl);
    free(uri);
    return pal_to_unix_errno(ret);
}

/* Open a PAL handle, and associate it with a LibOS handle (if provided). */
static int chroot_do_open(struct libos_handle* hdl, struct libos_dentry* dent, mode_t type,
                          int flags, mode_t perm) {
    assert(locked(&g_dcache_lock));

    int ret;

    char* uri;
    ret = dentry_uri(dent, type, &uri);
    if (ret < 0)
        return ret;

    PAL_HANDLE palhdl;
    enum pal_access access = LINUX_OPEN_FLAGS_TO_PAL_ACCESS(flags);
    enum pal_create_mode create = LINUX_OPEN_FLAGS_TO_PAL_CREATE(flags);
    pal_stream_options_t options = LINUX_OPEN_FLAGS_TO_PAL_OPTIONS(flags);
    mode_t host_perm = HOST_PERM(perm);
    ret = PalStreamOpen(uri, access, host_perm, create, options, &palhdl);
    if (ret < 0) {
        ret = pal_to_unix_errno(ret);
        goto out;
    }

    if (hdl) {
        hdl->uri = uri;
        uri = NULL;

        hdl->type = TYPE_CHROOT;
        hdl->seekable = true;
        hdl->pos = 0;
        hdl->pal_handle = palhdl;
    } else {
        PalObjectDestroy(palhdl);
    }
    ret = 0;

out:
    free(uri);
    return ret;
}

static int chroot_open(struct libos_handle* hdl, struct libos_dentry* dent, int flags) {
    assert(locked(&g_dcache_lock));
    assert(dent->inode);

    return chroot_do_open(hdl, dent, dent->inode->type, flags, /*perm=*/0);
}

static int chroot_creat(struct libos_handle* hdl, struct libos_dentry* dent, int flags, mode_t perm) {
    assert(locked(&g_dcache_lock));
    assert(!dent->inode);

    int ret;

    mode_t type = S_IFREG;

    ret = chroot_do_open(hdl, dent, type, flags | O_CREAT | O_EXCL, perm);
    if (ret < 0)
        return ret;

    return chroot_setup_dentry(dent, type, perm, /*size=*/0);
}

static int chroot_mkdir(struct libos_dentry* dent, mode_t perm) {
    assert(locked(&g_dcache_lock));
    assert(!dent->inode);

    int ret;

    mode_t type = S_IFDIR;

    ret = chroot_do_open(/*hdl=*/NULL, dent, type, O_CREAT | O_EXCL, perm);
    if (ret < 0)
        return ret;

    return chroot_setup_dentry(dent, type, perm, /*size=*/0);
}

static int chroot_flush(struct libos_handle* hdl) {
    assert(hdl->type == TYPE_CHROOT);

    int ret = PalStreamFlush(hdl->pal_handle);
    return pal_to_unix_errno(ret);
}

static ssize_t chroot_read(struct libos_handle* hdl, void* buf, size_t count, file_off_t* pos) {
    assert(hdl->type == TYPE_CHROOT);

    size_t actual_count = count;
    int ret = PalStreamRead(hdl->pal_handle, *pos, &actual_count, buf);
    if (ret < 0) {
        return pal_to_unix_errno(ret);
    }
    assert(actual_count <= count);
    if (hdl->inode->type == S_IFREG) {
        *pos += actual_count;
    }
    return actual_count;
}

static ssize_t chroot_write(struct libos_handle* hdl, const void* buf, size_t count,
                            file_off_t* pos) {
    assert(hdl->type == TYPE_CHROOT);

    size_t actual_count = count;
    int ret = PalStreamWrite(hdl->pal_handle, *pos, &actual_count, (void*)buf);
    if (ret < 0) {
        return pal_to_unix_errno(ret);
    }
    assert(actual_count <= count);
    if (hdl->inode->type == S_IFREG) {
        *pos += actual_count;
        uint64_t time_us;
        bool have_time = PalSystemTimeQuery(&time_us) >= 0;
        /* Update cached metadata after a host-backed write. */
        lock(&hdl->inode->lock);
        if (hdl->inode->size < *pos)
            hdl->inode->size = *pos;
        if (have_time) {
            hdl->inode->mtime = time_us / 1000000;
            hdl->inode->mtime_nsec = (time_us % 1000000) * 1000;
        }
        unlock(&hdl->inode->lock);
    }

    /* If there are any MAP_SHARED mappings for the file, this will read data from `hdl`. */
    if (__atomic_load_n(&hdl->inode->num_mmapped, __ATOMIC_ACQUIRE) != 0) {
        ret = reload_mmaped_from_file_handle(hdl);
        if (ret < 0) {
            log_error("reload mmapped regions of file failed: %s", unix_strerror(ret));
            BUG();
        }
    }

    return (ssize_t)actual_count;
}

static int chroot_mmap(struct libos_handle* hdl, void* addr, size_t size, int prot, int flags,
                       uint64_t offset) {
    assert(hdl->type == TYPE_CHROOT);
    assert(addr);

    // log_always("[mmap] file mapping request: addr=%p, length=%zu, prot=%d, flags=%d, offset=%lu",
    //            addr, size, prot, flags, offset);

    pal_prot_flags_t pal_prot = LINUX_PROT_TO_PAL(prot, flags);

    if (flags & MAP_ANONYMOUS)
        return -EINVAL;

    int ret = PalStreamMap(hdl->pal_handle, addr, pal_prot, offset, size);
    if (ret < 0)
        return pal_to_unix_errno(ret);

    return 0;
}

int chroot_readdir(struct libos_dentry* dent, readdir_callback_t callback, void* arg) {
    int ret;
    PAL_HANDLE palhdl;
    char* buf = NULL;
    size_t buf_size = READDIR_BUF_SIZE;

    assert(dent->inode->type == S_IFDIR);
    ret = chroot_temp_open(dent, &palhdl);
    if (ret < 0)
        return ret;

    buf = malloc(buf_size);
    if (!buf) {
        ret = -ENOMEM;
        goto out;
    }

    while (true) {
        size_t read_size = buf_size;
        ret = PalStreamRead(palhdl, /*offset=*/0, &read_size, buf);
        if (ret < 0) {
            ret = pal_to_unix_errno(ret);
            goto out;
        }
        if (read_size == 0) {
            /* End of directory listing */
            break;
        }

        /* Last entry must be null-terminated */
        assert(buf[read_size - 1] == '\0');

        /* Read all entries (separated by null bytes) and invoke `callback` on each */
        size_t start = 0;
        while (start < read_size - 1) {
            size_t end = start + strlen(&buf[start]);

            if (end == start) {
                log_error("chroot_readdir: empty name returned from PAL");
                BUG();
            }

            /* By the PAL convention, if a name ends with '/', it is a directory. However, we ignore
             * that distinction here and pass the name without '/' to the callback. */
            if (buf[end - 1] == '/')
                buf[end - 1] = '\0';

            if ((ret = callback(&buf[start], arg)) < 0)
                goto out;

            start = end + 1;
        }
    }
    ret = 0;

out:
    free(buf);
    PalObjectDestroy(palhdl);
    return ret;
}

int chroot_host_readlink(struct libos_dentry* dent, char* buf, size_t bufsize) {
    assert(locked(&g_dcache_lock));
    assert(dent->inode);

    char* uri;
    int ret = dentry_uri(dent, dent->inode->type, &uri);
    if (ret < 0)
        return ret;

    size_t size = bufsize;
    ret = PalHostReadlink(uri, buf, &size);
    free(uri);
    if (ret < 0)
        return pal_to_unix_errno(ret);
    return (int)size;
}

static int chroot_unlink(struct libos_dentry* dent) {
    assert(locked(&g_dcache_lock));
    assert(dent->inode);

    int ret;

    if (dent->inode->type == S_IFLNK) {
        char* uri = NULL;
        ret = dentry_uri(dent, S_IFREG, &uri);
        if (ret < 0)
            return ret;
        ret = PalFileUnlink(uri);
        free(uri);
        return ret < 0 ? pal_to_unix_errno(ret) : 0;
    }

    PAL_HANDLE palhdl;
    ret = chroot_temp_open(dent, &palhdl);
    if (ret < 0)
        return ret;

    ret = PalStreamDelete(palhdl, PAL_DELETE_ALL);
    PalObjectDestroy(palhdl);
    if (ret < 0)
        return pal_to_unix_errno(ret);

    return 0;
}

static int chroot_symlink(const char* target, struct libos_dentry* dent) {
    assert(locked(&g_dcache_lock));
    assert(!dent->inode);

    char* target_copy = strdup(target);
    if (!target_copy)
        return -ENOMEM;
    char* uri = NULL;
    /* PAL file URIs describe the backing object class and have no symlink URI type. */
    int ret = dentry_uri(dent, S_IFREG, &uri);
    if (ret < 0)
        goto out;
    ret = chroot_setup_dentry(dent, S_IFLNK, PERM_rwxrwxrwx, strlen(target));
    if (ret < 0)
        goto out;
    ret = PalFileCreateSymlink(target, uri);
    if (ret < 0) {
        ret = pal_to_unix_errno(ret);
        put_inode(dent->inode);
        dent->inode = NULL;
        goto out;
    }
    dent->inode->data = target_copy;
    target_copy = NULL;
out:
    free(target_copy);
    free(uri);
    return ret;
}

static int chroot_link(struct libos_dentry* old, struct libos_dentry* new) {
    assert(locked(&g_dcache_lock));
    assert(old->inode && !new->inode);

    char* old_uri = NULL;
    char* new_uri = NULL;
    mode_t uri_type = old->inode->type == S_IFDIR ? S_IFDIR : S_IFREG;
    int ret = dentry_uri(old, uri_type, &old_uri);
    if (ret < 0)
        goto out;
    ret = dentry_uri(new, uri_type, &new_uri);
    if (ret < 0)
        goto out;
    ret = PalFileCreateLink(old_uri, new_uri);
    if (ret < 0) {
        ret = pal_to_unix_errno(ret);
        goto out;
    }
    get_inode(old->inode);
    new->inode = old->inode;
    lock(&old->inode->lock);
    old->inode->nlink++;
    unlock(&old->inode->lock);
out:
    free(old_uri);
    free(new_uri);
    return ret;
}

static int chroot_follow_link(struct libos_dentry* dent, char** out_target) {
    if (dent->inode->type != S_IFLNK || !dent->inode->data)
        return -EINVAL;
    *out_target = strdup(dent->inode->data);
    return *out_target ? 0 : -ENOMEM;
}

static void chroot_idrop(struct libos_inode* inode) {
    if (inode->type == S_IFLNK)
        free(inode->data);
}

static int chroot_rename(struct libos_dentry* old, struct libos_dentry* new) {
    assert(locked(&g_dcache_lock));
    assert(old->inode);

    int ret;
    char* new_uri = NULL;

    if (old->inode->type == S_IFLNK) {
        char* old_uri = NULL;
        ret = dentry_uri(old, S_IFREG, &old_uri);
        if (ret < 0)
            return ret;
        ret = dentry_uri(new, S_IFREG, &new_uri);
        if (ret < 0) {
            free(old_uri);
            return ret;
        }
        ret = PalFileRename(old_uri, new_uri);
        free(old_uri);
        free(new_uri);
        return ret < 0 ? pal_to_unix_errno(ret) : 0;
    }

    mode_t uri_type = old->inode->type == S_IFDIR ? S_IFDIR : S_IFREG;
    ret = dentry_uri(new, uri_type, &new_uri);
    if (ret < 0)
        goto out;

    PAL_HANDLE palhdl;
    ret = chroot_temp_open(old, &palhdl);
    if (ret < 0)
        goto out;

    ret = PalStreamChangeName(palhdl, new_uri);
    PalObjectDestroy(palhdl);
    if (ret < 0) {
        ret = pal_to_unix_errno(ret);
        goto out;
    }
    ret = 0;

out:
    free(new_uri);
    return ret;
}

static int chroot_chmod(struct libos_dentry* dent, mode_t perm) {
    assert(locked(&g_dcache_lock));
    assert(dent->inode);

    int ret;

    PAL_HANDLE palhdl;
    ret = chroot_temp_open(dent, &palhdl);
    if (ret < 0)
        return ret;

    mode_t host_perm = HOST_PERM(perm);
    PAL_STREAM_ATTR attr = {.share_flags = host_perm};
    ret = PalStreamAttributesSetByHandle(palhdl, &attr);
    PalObjectDestroy(palhdl);
    if (ret < 0)
        return pal_to_unix_errno(ret);

    return 0;
}

static int chroot_set_times(struct libos_dentry* dent, const struct __kernel_timespec times[2]) {
    assert(locked(&g_dcache_lock));
    assert(dent->inode);

    if (times[0].tv_nsec == UTIME_OMIT && times[1].tv_nsec == UTIME_OMIT)
        return 0;

    PAL_HANDLE palhdl;
    int ret = chroot_temp_open(dent, &palhdl);
    if (ret < 0)
        return ret;

    PAL_STREAM_ATTR attr = {0};
    if (times[0].tv_nsec != UTIME_OMIT) {
        attr.set_atime = true;
        attr.atime_now = times[0].tv_nsec == UTIME_NOW;
        if (!attr.atime_now)
            attr.atime = (PAL_TIMESPEC){.tv_sec = times[0].tv_sec, .tv_nsec = times[0].tv_nsec};
    }
    if (times[1].tv_nsec != UTIME_OMIT) {
        attr.set_mtime = true;
        attr.mtime_now = times[1].tv_nsec == UTIME_NOW;
        if (!attr.mtime_now)
            attr.mtime = (PAL_TIMESPEC){.tv_sec = times[1].tv_sec, .tv_nsec = times[1].tv_nsec};
    }
    ret = PalStreamAttributesSetByHandle(palhdl, &attr);
    PalObjectDestroy(palhdl);
    if (ret < 0)
        return pal_to_unix_errno(ret);

    uint64_t now_us;
    if (PalSystemTimeQuery(&now_us) < 0)
        return -EPERM;
    time_t now = now_us / 1000000;
    long now_nsec = (now_us % 1000000) * 1000;
    lock(&dent->inode->lock);
    if (times[0].tv_nsec != UTIME_OMIT) {
        dent->inode->atime = times[0].tv_nsec == UTIME_NOW ? now : times[0].tv_sec;
        /* The host file stores nanosecond precision; the PAL time query is microsecond based, so
         * `UTIME_NOW` here is accurate to the microsecond. */
        dent->inode->atime_nsec = times[0].tv_nsec == UTIME_NOW ? now_nsec : times[0].tv_nsec;
    }
    if (times[1].tv_nsec != UTIME_OMIT) {
        dent->inode->mtime = times[1].tv_nsec == UTIME_NOW ? now : times[1].tv_sec;
        dent->inode->mtime_nsec = times[1].tv_nsec == UTIME_NOW ? now_nsec : times[1].tv_nsec;
    }
    dent->inode->ctime = now;
    dent->inode->ctime_nsec = now_nsec;
    unlock(&dent->inode->lock);
    return 0;
}

static int chroot_fchmod(struct libos_handle* hdl, mode_t perm) {
    int ret;

    mode_t host_perm = HOST_PERM(perm);
    PAL_STREAM_ATTR attr = {.share_flags = host_perm};
    ret = PalStreamAttributesSetByHandle(hdl->pal_handle, &attr);
    if (ret < 0)
        return pal_to_unix_errno(ret);

    return 0;
}

struct libos_fs_ops chroot_fs_ops = {
    .mount      = &chroot_mount,
    .flush      = &chroot_flush,
    .read       = &chroot_read,
    .write      = &chroot_write,
    .mmap       = &chroot_mmap,
    /* TODO: this function emulates lseek() completely inside the LibOS, but some device files may
     * report size == 0 during fstat() and may provide device-specific lseek() logic; this emulation
     * breaks for such device-specific cases */
    .seek       = &generic_inode_seek,
    .hstat      = &generic_inode_hstat,
    .truncate   = &generic_truncate,
    .poll       = &generic_inode_poll,
    .fchmod     = &chroot_fchmod,
};

struct libos_d_ops chroot_d_ops = {
    .open    = &chroot_open,
    .lookup  = &chroot_lookup,
    .creat   = &chroot_creat,
    .mkdir   = &chroot_mkdir,
    .stat    = &generic_inode_stat,
    .readdir = &chroot_readdir,
    .unlink  = &chroot_unlink,
    .rename  = &chroot_rename,
    .chmod   = &chroot_chmod,
    .set_times = &chroot_set_times,
    .symlink = &chroot_symlink,
    .link = &chroot_link,
    .follow_link = &chroot_follow_link,
    .idrop = &chroot_idrop,
};

struct libos_fs chroot_builtin_fs = {
    .name   = "chroot",
    .fs_ops = &chroot_fs_ops,
    .d_ops  = &chroot_d_ops,
};
