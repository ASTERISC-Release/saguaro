/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2014 Stony Brook University */

/*
 * Implementation of system calls "stat", "lstat", "fstat" and "readlink".
 */


#include "libos_fs.h"
#include "libos_graphics.h"
#include "libos_handle.h"
#include "libos_internal.h"
#include "libos_process.h"
#include "libos_table.h"
#include "linux_abi/errors.h"
#include "linux_abi/fs.h"
#include "stat.h"
static int do_stat(struct libos_dentry* dent, struct stat* stat) {
    assert(locked(&g_dcache_lock));
    assert(dent->inode);

    struct libos_fs* fs = dent->inode->fs;

    if (!fs || !fs->d_ops || !fs->d_ops->stat)
        return -EACCES;

    int ret = fs->d_ops->stat(dent, stat);
    if (ret < 0)
        return ret;

    /* Update `st_ino` from dentry */
    stat->st_ino = dentry_ino(dent);
    return 0;
}

static int do_hstat(struct libos_handle* hdl, struct stat* stat) {
    struct libos_fs* fs = hdl->fs;

    if (!fs || !fs->fs_ops || !fs->fs_ops->hstat)
        return -EACCES;

    int ret = fs->fs_ops->hstat(hdl, stat);
    if (ret < 0)
        return ret;

    /* Update `st_ino` from dentry */
    if (hdl->dentry)
        stat->st_ino = dentry_ino(hdl->dentry);

    return 0;
}

long libos_syscall_stat(const char* file, struct stat* stat) {
    if (!is_user_string_readable(file))
        return -EFAULT;

    if (!is_user_memory_writable(stat, sizeof(*stat)))
        return -EFAULT;

    int ret;
    struct libos_dentry* dent = NULL;

    lock(&g_dcache_lock);
    ret = path_lookupat(/*start=*/NULL, file, LOOKUP_FOLLOW, &dent);
    if (ret < 0)
        goto out;

    ret = do_stat(dent, stat);
out:
    unlock(&g_dcache_lock);
    if (dent)
        put_dentry(dent);
    return ret;
}

long libos_syscall_lstat(const char* file, struct stat* stat) {
    if (!is_user_string_readable(file))
        return -EFAULT;

    if (!is_user_memory_writable(stat, sizeof(*stat)))
        return -EFAULT;

    int ret;
    struct libos_dentry* dent = NULL;

    lock(&g_dcache_lock);
    ret = path_lookupat(/*start=*/NULL, file, LOOKUP_NO_FOLLOW, &dent);
    if (ret < 0)
        goto out;

    ret = do_stat(dent, stat);
out:
    unlock(&g_dcache_lock);
    if (dent)
        put_dentry(dent);
    return ret;
}

long libos_syscall_fstat(int fd, struct stat* stat) {
    struct libos_handle* hdl = get_fd_handle(fd, NULL, NULL);
    if (!hdl)
        return -EBADF;

    if (!is_user_memory_writable(stat, sizeof(*stat)))
        return -EFAULT;

    #ifdef ENABLE_GRAPHICS
        if(hdl->type == TYPE_HOST || hdl->type == TYPE_HOST_IDENTITY)
        {
            /* a forwarded host object has no `fs`; an identity handle sits at the raw host
             * number and the listener subtracts fs_offset (as for lseek) */
            uint64_t host_arg = hdl->type == TYPE_HOST_IDENTITY ? (uint64_t)fd + fs_offset
                                                                 : (uint64_t)fd;
            long _offset = libos_syscall_acquire_libos_lock();
            comm_page_t* c = comm_page(_offset);
            struct gbounce* b = graphics_bounce(_offset);
            uint64_t stat_pa;
            void* out = graphics_bounce_out(b, stat, sizeof(*stat), &stat_pa);
            if (!out) {
                libos_syscall_relinquish_libos_lock(_offset);
                put_handle(hdl);
                return -ENOMEM;
            }
            c->p1 = host_arg;
            c->p2 = stat_pa;
            __sync_synchronize();
            c->req_bit = FSTAT;
            put_handle(hdl);

            long retval = comm_sync_notify(c);
            if (retval == 0 && out != stat)
                memcpy(stat, out, sizeof(*stat));
            libos_syscall_relinquish_libos_lock(_offset);
            return retval;
        }  
    #endif

    int ret = do_hstat(hdl, stat);
    put_handle(hdl);
    return ret;
}

long libos_syscall_readlinkat(int dirfd, const char* file, char* buf, int bufsize) {
    int ret;
    char* target = NULL;

    if (!is_user_string_readable(file))
        return -EFAULT;

    if (bufsize <= 0)
        return -EINVAL;

    if (!is_user_memory_writable(buf, bufsize))
        return -EFAULT;

    struct libos_dentry* dent = NULL;
    struct libos_dentry* dir = NULL;

    if (*file != '/' && (ret = get_dirfd_dentry(dirfd, &dir)) < 0)
        goto out;

    lock(&g_dcache_lock);
    ret = path_lookupat(dir, file, LOOKUP_NO_FOLLOW, &dent);
    if (ret < 0)
        goto out;

    ret = -EINVAL;
    /* The correct behavior is to return -EINVAL if file is not a
       symbolic link */
    if (dent->inode->type != S_IFLNK) {
        /* A chroot mount with `host_symlinks`: the host resolved the link during lookup, so the
         * dentry has the type of the target; ask the host whether the path is a link. */
        if (dent->mount && dent->mount->host_symlinks && dent->mount->fs == &chroot_builtin_fs)
            ret = chroot_host_readlink(dent, buf, bufsize);
        goto out;
    }

    struct libos_fs* fs = dent->inode->fs;
    if (!fs->d_ops || !fs->d_ops->follow_link)
        goto out;

    ret = fs->d_ops->follow_link(dent, &target);
    if (ret < 0)
        goto out;

    size_t target_len = strlen(target);

    ret = bufsize;
    if (target_len < (size_t)bufsize)
        ret = target_len;

    memcpy(buf, target, ret);
out:
    unlock(&g_dcache_lock);
    if (dent) {
        put_dentry(dent);
    }
    if (dir) {
        put_dentry(dir);
    }
    free(target);
    return ret;
}

long libos_syscall_readlink(const char* file, char* buf, int bufsize) {
    #ifdef ENABLE_GRAPHICS
        if(graphics_redirect_enabled() && strstr(file, "/sys/")){
            long _offset = libos_syscall_acquire_libos_lock();
            comm_page_t* c = comm_page(_offset);
            struct gbounce* b = graphics_bounce(_offset);
            uint64_t path_pa = graphics_bounce_in(b, file, strlen(file) + 1);
            size_t chunk = bufsize > 0 ? MIN((size_t)bufsize, graphics_bounce_room(b)) : 0;
            uint64_t buf_pa;
            void* out = graphics_bounce_out(b, buf, chunk, &buf_pa);
            if (!path_pa || !out) {
                libos_syscall_relinquish_libos_lock(_offset);
                return -ENAMETOOLONG;
            }
            c->p1 = path_pa;
            c->p2 = buf_pa;
            c->p3 = (uint64_t) chunk;

            __sync_synchronize();
            c->req_bit = READLINK;
            long retval = comm_sync_notify(c);
            if (retval > 0 && out != buf)
                memcpy(buf, out, MIN((size_t)retval, chunk));
            libos_syscall_relinquish_libos_lock(_offset);
            return retval;
        }
    #endif
    return libos_syscall_readlinkat(AT_FDCWD, file, buf, bufsize);
}

static int __do_statfs(struct libos_mount* mount, struct statfs* buf) {
    __UNUSED(mount);
    if (!is_user_memory_writable(buf, sizeof(*buf)))
        return -EFAULT;

    memset(buf, 0, sizeof(*buf));

    buf->f_bsize  = 4096;
    buf->f_blocks = 20000000;
    buf->f_bfree  = 10000000;
    buf->f_bavail = 10000000;

    log_debug("statfs: %ld %ld %ld", buf->f_blocks, buf->f_bfree, buf->f_bavail);

    return 0;
}

long libos_syscall_statfs(const char* path, struct statfs* buf) {
    if (!is_user_string_readable(path))
        return -EFAULT;

    int ret;
    struct libos_dentry* dent = NULL;

    lock(&g_dcache_lock);
    ret = path_lookupat(/*start=*/NULL, path, LOOKUP_FOLLOW, &dent);
    unlock(&g_dcache_lock);
    if (ret < 0)
        return ret;

    struct libos_mount* mount = dent->mount;
    put_dentry(dent);
    return __do_statfs(mount, buf);
}

long libos_syscall_fstatfs(int fd, struct statfs* buf) {
    struct libos_handle* hdl = get_fd_handle(fd, NULL, NULL);
    if (!hdl)
        return -EBADF;

    struct libos_mount* mount = hdl->dentry ? hdl->dentry->mount : NULL;
    put_handle(hdl);
    return __do_statfs(mount, buf);
}

/*
 * Handle the special case of `fstatat` with empty path (permitted with AT_EMPTY_PATH). Note that in
 * this case `dirfd` can point to a non-directory file, so we cannot use `get_dirfd_dentry`.
 */
static int do_fstatat_empty_path(int dirfd, struct stat* statbuf) {
    if (dirfd != AT_FDCWD)
        return libos_syscall_fstat(dirfd, statbuf);

    lock(&g_process.fs_lock);
    struct libos_dentry* dent = g_process.cwd;
    get_dentry(dent);
    unlock(&g_process.fs_lock);

    lock(&g_dcache_lock);

    int ret;

    if (!dent->inode) {
        ret = -ENOENT;
        goto out;
    }

    ret = do_stat(dent, statbuf);
out:
    unlock(&g_dcache_lock);
    put_dentry(dent);
    return ret;
}

long libos_syscall_newfstatat(int dirfd, const char* pathname, struct stat* statbuf, int flags) {
    #ifdef ENABLE_GRAPHICS
        if(graphics_redirect_enabled() && (strstr(pathname, "/dev") || strstr(pathname, "/sys") || strstr(pathname, "/proc/sys"))){
            if(!strstr(pathname, "devtest")){
                long _offset = libos_syscall_acquire_libos_lock();
                comm_page_t* c = comm_page(_offset);
                struct gbounce* b = graphics_bounce(_offset);
                uint64_t path_pa = graphics_bounce_in(b, pathname, strlen(pathname) + 1);
                uint64_t stat_pa;
                void* out = graphics_bounce_out(b, statbuf, sizeof(*statbuf), &stat_pa);
                if (!path_pa || !out) {
                    libos_syscall_relinquish_libos_lock(_offset);
                    return -ENAMETOOLONG;
                }
                c->p1 = (uint64_t) dirfd;
                c->p2 = path_pa;
                c->p3 = stat_pa;
                c->p4 = (uint64_t) flags;

                __sync_synchronize();
                c->req_bit = NEWFSTAT;
                long retval = comm_sync_notify(c);
                if (retval == 0 && out != statbuf)
                    memcpy(statbuf, out, sizeof(*statbuf));
                libos_syscall_relinquish_libos_lock(_offset);
                return retval;
            }
        }
    #endif

    if (flags & ~(AT_EMPTY_PATH | AT_NO_AUTOMOUNT | AT_SYMLINK_NOFOLLOW))
        return -EINVAL;
    if (!is_user_string_readable(pathname))
        return -EFAULT;
    if (!is_user_memory_writable(statbuf, sizeof(*statbuf)))
        return -EFAULT;

    int lookup_flags = LOOKUP_FOLLOW;
    if (flags & AT_SYMLINK_NOFOLLOW)
        lookup_flags &= ~LOOKUP_FOLLOW;
    if (flags & AT_NO_AUTOMOUNT) {
        /* Do nothing as automount isn't supported */
        log_warning("newfstatat: ignoring AT_NO_AUTOMOUNT.");
    }

    if (!*pathname) {
        if (!(flags & AT_EMPTY_PATH))
            return -ENOENT;

        return do_fstatat_empty_path(dirfd, statbuf);
    }

    int ret;

    struct libos_dentry* dir = NULL;
    if (*pathname != '/') {
        ret = get_dirfd_dentry(dirfd, &dir);
        if (ret < 0)
            return ret;
    }

    lock(&g_dcache_lock);

    struct libos_dentry* dent = NULL;
    ret = path_lookupat(dir, pathname, lookup_flags, &dent);
    if (ret < 0)
        goto out;

    ret = do_stat(dent, statbuf);
out:
    unlock(&g_dcache_lock);
    if (dent)
        put_dentry(dent);
    if (dir)
        put_dentry(dir);
    return ret;
}

/*
 * `statx` is `newfstatat` with a different result layout. It is needed on architectures whose glibc
 * assumes the kernel has it and so does not fall back to `newfstatat` on ENOSYS (riscv64); see the
 * syscall tables for where it is enabled. The structures are spelled out here because
 * <linux/stat.h> cannot be included together with our "stat.h".
 */
struct libos_statx_timestamp {
    int64_t tv_sec;
    uint32_t tv_nsec;
    int32_t __reserved;
};

struct libos_statx {
    uint32_t stx_mask;
    uint32_t stx_blksize;
    uint64_t stx_attributes;
    uint32_t stx_nlink;
    uint32_t stx_uid;
    uint32_t stx_gid;
    uint16_t stx_mode;
    uint16_t __spare0[1];
    uint64_t stx_ino;
    uint64_t stx_size;
    uint64_t stx_blocks;
    uint64_t stx_attributes_mask;
    struct libos_statx_timestamp stx_atime;
    struct libos_statx_timestamp stx_btime;
    struct libos_statx_timestamp stx_ctime;
    struct libos_statx_timestamp stx_mtime;
    uint32_t stx_rdev_major;
    uint32_t stx_rdev_minor;
    uint32_t stx_dev_major;
    uint32_t stx_dev_minor;
    uint64_t __spare2[14];
};
static_assert(sizeof(struct libos_statx) == 256, "struct statx is 256 bytes in the Linux ABI");

#define LIBOS_STATX_BASIC_STATS   0x000007ffU
#define LIBOS_STATX__RESERVED     0x80000000U
#define LIBOS_AT_STATX_SYNC_TYPE  0x6000

/* The encoding of `dev_t` that the kernel hands to userspace (new_encode_dev() in Linux). */
static uint32_t dev_major(uint64_t dev) {
    return (dev & 0xfff00) >> 8;
}

static uint32_t dev_minor(uint64_t dev) {
    return (dev & 0xff) | ((dev >> 12) & 0xfff00);
}

long libos_syscall_statx(int dirfd, const char* pathname, unsigned int flags, unsigned int mask,
                         void* buf) {
    struct libos_statx* statxbuf = buf;

    if (flags & ~(AT_EMPTY_PATH | AT_NO_AUTOMOUNT | AT_SYMLINK_NOFOLLOW | LIBOS_AT_STATX_SYNC_TYPE))
        return -EINVAL;
    if ((flags & LIBOS_AT_STATX_SYNC_TYPE) == LIBOS_AT_STATX_SYNC_TYPE)
        return -EINVAL;
    if (mask & LIBOS_STATX__RESERVED)
        return -EINVAL;
    if (!is_user_memory_writable(statxbuf, sizeof(*statxbuf)))
        return -EFAULT;

    /* Automount is not supported and the sync type has no meaning here, so both are dropped.
     * `newfstatat` only accepts a buffer in application memory, so it first writes its (smaller)
     * result into the caller's buffer, from where it is copied out and converted. */
    static_assert(sizeof(struct stat) <= sizeof(*statxbuf), "struct stat must fit into statx");
    long ret = libos_syscall_newfstatat(dirfd, pathname, (struct stat*)statxbuf,
                                        flags & (AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW));
    if (ret < 0)
        return ret;
    struct stat st;
    memcpy(&st, statxbuf, sizeof(st));

    memset(statxbuf, 0, sizeof(*statxbuf));
    statxbuf->stx_mask          = LIBOS_STATX_BASIC_STATS;
    statxbuf->stx_blksize       = st.st_blksize;
    statxbuf->stx_nlink         = st.st_nlink;
    statxbuf->stx_uid           = st.st_uid;
    statxbuf->stx_gid           = st.st_gid;
    statxbuf->stx_mode          = st.st_mode;
    statxbuf->stx_ino           = st.st_ino;
    statxbuf->stx_size          = st.st_size;
    statxbuf->stx_blocks        = st.st_blocks;
    statxbuf->stx_atime.tv_sec  = st.st_atime;
    statxbuf->stx_atime.tv_nsec = st.st_atime_nsec;
    statxbuf->stx_ctime.tv_sec  = st.st_ctime;
    statxbuf->stx_ctime.tv_nsec = st.st_ctime_nsec;
    statxbuf->stx_mtime.tv_sec  = st.st_mtime;
    statxbuf->stx_mtime.tv_nsec = st.st_mtime_nsec;
    statxbuf->stx_rdev_major    = dev_major(st.st_rdev);
    statxbuf->stx_rdev_minor    = dev_minor(st.st_rdev);
    statxbuf->stx_dev_major     = dev_major(st.st_dev);
    statxbuf->stx_dev_minor     = dev_minor(st.st_dev);
    return 0;
}
