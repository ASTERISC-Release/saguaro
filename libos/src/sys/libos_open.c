/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2014 Stony Brook University */

/*
 * Implementation of system calls: "read", "write", "open", "creat", "openat", "close", "lseek",
 * "pread64", "pwrite64", "getdents", "getdents64", "fsync", "truncate" and "ftruncate".
 */

#define _POSIX_C_SOURCE 200809L  /* for SSIZE_MAX */
#include <limits.h>
#include <stdalign.h>
#include <linux/falloc.h>

#include "libos_fs.h"
#include "libos_graphics.h"
#include "libos_handle.h"
#include "libos_inotify.h"
#include "libos_internal.h"
#include "libos_lock.h"
#include "libos_process.h"
#include "libos_signal.h"
#include "libos_table.h"
#include "libos_thread.h"
#include "linux_abi/limits.h"
#include "linux_abi/errors.h"
#include "linux_abi/fs.h"
#include "linux_abi/memory.h"
#include "stat.h"
ssize_t do_handle_read(struct libos_handle* hdl, void* buf, size_t count) {
    if (!(hdl->acc_mode & MAY_READ))
        return -EBADF;

    struct libos_fs* fs = hdl->fs;
    assert(fs && fs->fs_ops);

    if (!fs->fs_ops->read)
        return -EBADF;

    if (hdl->is_dir)
        return -EISDIR;

    maybe_lock_pos_handle(hdl);
    ssize_t ret = fs->fs_ops->read(hdl, buf, count, &hdl->pos);
    maybe_unlock_pos_handle(hdl);
    return ret;
}

/*
 * A memfd reopened through /proc/self/fd/N shares one handle with the original descriptor but may
 * have a different access mode (the kernel keeps the mode per open file description); the mode is
 * recorded on the fd table entry, so enforce it there for memfd descriptors. Other file
 * descriptions of the same handle are unaffected (their entry carries the handle's own mode).
 */
static bool fd_memfd_access_denied(int fd, struct libos_handle* hdl, int mask) {
    if (!hdl->is_memfd)
        return false;
    int mode = get_fd_acc_mode((uint32_t)fd, /*handle_map=*/NULL);
    return mode >= 0 && !(mode & mask);
}

long libos_syscall_read(int fd, void* buf, size_t count) {
    long retval;
    if (!is_user_memory_writable(buf, count))
        return -EFAULT;

    struct libos_handle* hdl = get_fd_handle(fd, NULL, NULL);
    if (!hdl)
        return -EBADF;

    if (fd_memfd_access_denied(fd, hdl, MAY_READ)) {
        put_handle(hdl);
        return -EBADF;
    }

    #ifdef ENABLE_GRAPHICS
        if(hdl->type == TYPE_HOST){
            log_debug("redirecting read(%d, 0x%lx, %d) to host", fd, buf, count);
            long _offset = libos_syscall_acquire_libos_lock();
            comm_page_t* c = comm_page(_offset);
            c->p1 = (uint64_t) fd;
            c->p2 = (uint64_t) buf;
            c->p3 = (uint64_t) count;
            c->req_bit = READ;
            put_handle(hdl);
            retval = (long)comm_sync_notify(c);
            libos_syscall_relinquish_libos_lock(_offset);
            return retval;
        }
    #endif

    ssize_t ret = do_handle_read(hdl, buf, count);
    if (ret > 0 && hdl->dentry && hdl->dentry->inode && S_ISREG(hdl->dentry->inode->type))
        libos_inotify_file_event(hdl->dentry, IN_ACCESS);
    put_handle(hdl);
    if (ret == -EINTR) {
        ret = -ERESTARTSYS;
    }
    return ret;
}

/* O_APPEND: every write of a regular file starts at its end (also pwrite(), as on Linux). The PAL
 * has no append mode, so the LibOS moves the position to the current size first; another
 * process appending to the same host file at the same moment is not serialised with this. */
int handle_append_position(struct libos_handle* hdl, file_off_t* pos) {
    if (!(hdl->flags & O_APPEND) || !hdl->inode || hdl->inode->type != S_IFREG)
        return 0;
    struct libos_fs* fs = hdl->fs;
    if (fs && fs->fs_ops && fs->fs_ops->hstat) {
        struct stat st;
        int ret = fs->fs_ops->hstat(hdl, &st);
        if (ret < 0)
            return ret;
        *pos = st.st_size;
        return 0;
    }
    lock(&hdl->inode->lock);
    *pos = hdl->inode->size;
    unlock(&hdl->inode->lock);
    return 0;
}

/* RLIMIT_FSIZE for a write of `*count` bytes at `pos` of a regular file: beyond the limit the
 * write is EFBIG and SIGXFSZ is sent, across it the write is shortened (LTP llseek01). */
int handle_fsize_limit(struct libos_handle* hdl, file_off_t pos, size_t* count) {
    if (!*count || !hdl->inode || hdl->inode->type != S_IFREG)
        return 0;
    uint64_t limit = get_rlimit_cur(RLIMIT_FSIZE);
    if (limit == RLIM_INFINITY)
        return 0;
    if ((uint64_t)pos >= limit) {
        siginfo_t info = {.si_signo = SIGXFSZ, .si_code = SI_KERNEL};
        (void)append_signal(get_cur_thread(), &info);
        return -EFBIG;
    }
    if ((uint64_t)pos + *count > limit)
        *count = limit - (uint64_t)pos;
    return 0;
}

ssize_t do_handle_write(struct libos_handle* hdl, const void* buf, size_t count) {
    int seal_ret = libos_memfd_check_write(hdl, hdl->pos, count);
    if (seal_ret < 0)
        return seal_ret;
    if (!(hdl->acc_mode & MAY_WRITE))
        return -EBADF;

    struct libos_fs* fs = hdl->fs;
    assert(fs && fs->fs_ops);

    if (!fs->fs_ops->write)
        return -EBADF;

    if (hdl->is_dir)
        return -EISDIR;

    maybe_lock_pos_handle(hdl);
    ssize_t ret = handle_append_position(hdl, &hdl->pos);
    if (ret == 0)
        ret = handle_fsize_limit(hdl, hdl->pos, &count);
    if (ret == 0)
        ret = fs->fs_ops->write(hdl, buf, count, &hdl->pos);
    maybe_unlock_pos_handle(hdl);
    return ret;
}

long libos_syscall_write(int fd, const void* buf, size_t count) {
    long retval;
    if (!is_user_memory_readable((void*)buf, count))
        return -EFAULT;

    struct libos_handle* hdl = get_fd_handle(fd, NULL, NULL);
    if (!hdl)
        return -EBADF;

    if (fd_memfd_access_denied(fd, hdl, MAY_WRITE)) {
        put_handle(hdl);
        return -EBADF;
    }

    #ifdef ENABLE_GRAPHICS
        if(hdl->type == TYPE_HOST){
            log_debug("redirecting write(%d, 0x%lx, %d) to host", fd, buf, count);
            long _offset = libos_syscall_acquire_libos_lock();
            comm_page_t* c = comm_page(_offset);
            c->p1 = (uint64_t) fd;
            c->p2 = (uint64_t) buf;
            c->p3 = (uint64_t) count;
            c->req_bit = WRITE;
            put_handle(hdl);

            retval = (long)comm_sync_notify(c);
            libos_syscall_relinquish_libos_lock(_offset);
            return retval;
        }
    #endif

    ssize_t ret = do_handle_write(hdl, buf, count);
    if (ret > 0 && hdl->dentry && hdl->dentry->inode && S_ISREG(hdl->dentry->inode->type))
        libos_inotify_file_event(hdl->dentry, IN_MODIFY);
    put_handle(hdl);
    if (ret == -EINTR) {
        ret = -ERESTARTSYS;
    }
    return ret;
}

long libos_syscall_open(const char* file, int flags, mode_t mode) {
    return libos_syscall_openat(AT_FDCWD, file, flags, mode);
}

long libos_syscall_creat(const char* path, mode_t mode) {
    return libos_syscall_open(path, O_CREAT | O_TRUNC | O_WRONLY, mode);
}

#ifdef ENABLE_GRAPHICS
/* The graphics comm window at COMM_ADDR (0xf00000) is a fixed host-shared page provided by the
 * full Tirith deployment: the VM/TDX PAL guest memory map has the sg-listener's shared memory
 * attached there by the host. Under the plain `Linux` PAL (any gramine-direct run, including
 * the entire aarch64 port, where `linux` is the only PAL) no host agent exists and the page
 * is *never* mapped -- and note comm_sync_notify()'s `magic` check cannot cover this, because
 * every caller writes c->p1..p3 *before* calling it, so the very first write faults
 * (SIGSEGV at 0xf00018) long before the magic read could answer "no listener". This used to
 * crash every openat() of any /dev or /sys path under the Linux PAL (the whole
 * shm/devfs/close_range/procstat/keys/poll regression cluster; the x86_64 CI presumably only
 * ever exercises these paths in VM/TDX deployments). graphics host redirection therefore must be
 * gated on the PAL host type at the single place where TYPE_HOST handles are created; every
 * other comm_page() writer in the LibOS (ioctl/fcntl/dup/mmap/stat) is keyed on
 * hdl->type == TYPE_HOST(_IDENTITY) and so becomes inert along with it. */
bool g_graphics_comm_mapped = false;

bool graphics_redirect_enabled(void) {
    /* The window is application memory until the graphics preload library maps it (see
     * libos_syscall_mmap()): an application that never does so, i.e. any non-graphics one, must
     * not have its /dev and /sys accesses redirected, or the first write to the window faults. */
    if (!__atomic_load_n(&g_graphics_comm_mapped, __ATOMIC_ACQUIRE)) {
        return false;
    }
    const char* host = g_pal_public_state->host_type;
    return !strcmp(host, "VM") || !strcmp(host, "TDX");
}

static bool is_graphics_host_path(const char* path) {
    if (!graphics_redirect_enabled()) {
        return false;
    }
    static const char* const roots[] = {"/dev", "/sys", "/root/.cache"};

    for (size_t i = 0; i < ARRAY_SIZE(roots); i++) {
        size_t len = strlen(roots[i]);
        if (strncmp(path, roots[i], len) == 0 && (path[len] == '\0' || path[len] == '/'))
            return true;
    }
    return false;
}
#endif

/* New system call added for the graphics implementation */
long libos_syscall_register_host_identity_fd(int fd) {
    struct libos_handle* hdl;
    long ret;
    log_always("Registering host fd=%d with Gramine", fd);

    /* Get a new handle for the provided fd*/
    hdl = get_new_handle();
    if (!hdl) {
        ret = -ENOMEM;
        goto out;
    }

    /* Mark this as a host identity handle type */
    hdl->type = TYPE_HOST_IDENTITY;
    ret = set_new_fd_handle_by_fd(fd, hdl, 0, NULL);
    put_handle(hdl);

out:
    return ret;
}

static long do_openat(int dfd, const char* filename, int flags, int mode, bool user_pointer) {
    struct libos_handle* hdl;
    int ret = 0;

    if (user_pointer && !is_user_string_readable(filename))
        return -EFAULT;

    /* procfs normally resolves /proc/self/fd/N through the target pathname. A memfd has no
     * pathname after creation, so reopen it by duplicating the live LibOS handle instead. */
    static const char proc_fd_prefix[] = "/proc/self/fd/";
    if (!strncmp(filename, proc_fd_prefix, sizeof(proc_fd_prefix) - 1) &&
        !(flags & ~(O_CLOEXEC | O_LARGEFILE | O_ACCMODE | O_CREAT | O_TRUNC))) {
        const char* p = filename + sizeof(proc_fd_prefix) - 1;
        if (*p >= '0' && *p <= '9') {
            unsigned long target_fd = 0;
            do {
                unsigned int digit = (unsigned int)(*p - '0');
                if (target_fd > (UINT32_MAX - digit) / 10)
                    return -EBADF;
                target_fd = target_fd * 10 + digit;
                p++;
            } while (*p >= '0' && *p <= '9');
            if (!*p) {
                struct libos_handle* target = get_fd_handle((int)target_fd, NULL, NULL);
                if (target && target->is_memfd) {
                    /* A memfd reopened through /proc/self/fd/N gets full read-write access (the
                     * documented memfd behavior — this is why the seals exist); after
                     * F_SEAL_FUTURE_WRITE a new writable descriptor is refused with EPERM. */
                    lock(&target->lock);
                    bool refuse_write = (target->memfd_seals & LIBOS_F_SEAL_FUTURE_WRITE)
                                        && (flags & O_ACCMODE) != O_RDONLY;
                    unlock(&target->lock);
                    if (refuse_write) {
                        put_handle(target);
                        return -EPERM;
                    }
                    if (flags & O_TRUNC) {
                        long trunc_ret = libos_syscall_ftruncate((int)target_fd, 0);
                        if (trunc_ret < 0) {
                            put_handle(target);
                            return trunc_ret;
                        }
                    }
                    int new_fd = set_new_fd_handle(target,
                        (flags & O_CLOEXEC) ? FD_CLOEXEC : 0, NULL);
                    int acc_mode = 0;
                    if ((flags & O_ACCMODE) != O_WRONLY)
                        acc_mode |= MAY_READ;
                    if ((flags & O_ACCMODE) != O_RDONLY)
                        acc_mode |= MAY_WRITE;
                    if (new_fd >= 0)
                        set_fd_acc_mode((uint32_t)new_fd, acc_mode, NULL);
                    put_handle(target);
                    return new_fd;
                }
                if (target)
                    put_handle(target);
            }
        }
    }

    /* Graphics handle to redirect open() system call */
#ifdef ENABLE_GRAPHICS
    long retval = 0;
    bool use_graphics_host = is_graphics_host_path(filename) && !strstr(filename, "devtest")
                       && !strstr(filename, "urandom");
    if (use_graphics_host) {
        log_debug("Device open(): %s\n", filename);
        long _offset = libos_syscall_acquire_libos_lock();
            comm_page_t* c = comm_page(_offset);
        c->p1 = (uint64_t)filename;
        c->p2 = (uint64_t)flags;
        c->p3 = (uint64_t)mode;
        c->req_bit = OPEN;

        retval = (long)comm_sync_notify(c);
        libos_syscall_relinquish_libos_lock(_offset);

        if (retval < 0) {
            ret = retval;
            goto out;
        }

        hdl = get_new_handle();
        if (!hdl) {
            ret = -ENOMEM;
            goto out;
        }
        goto graphics_cont;
    }
#endif

    /* Clear invalid flags. */
    flags &= O_ACCMODE | O_APPEND |  O_CLOEXEC | O_CREAT | O_DIRECT | O_DIRECTORY | O_DSYNC | O_EXCL
             | O_LARGEFILE | O_NOATIME | O_NOCTTY | O_NOFOLLOW | O_NONBLOCK | O_PATH | O_SYNC
             | O_TMPFILE | O_TRUNC;

    if (   (flags & O_ACCMODE) != O_RDONLY
        && (flags & O_ACCMODE) != O_WRONLY
        && (flags & O_ACCMODE) != O_RDWR)
        return -EINVAL;

    /* TODO: fail explicitly on valid but unsupported flags. */

    if (flags & O_PATH) {
        flags &= O_PATH | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW;
    }

    if (!(flags & O_CREAT)) {
        /* `mode` should be ignored if O_CREAT is not specified, according to man */
        mode = 0;
    } else {
        lock(&g_process.fs_lock);
        mode_t umask = g_process.umask;
        unlock(&g_process.fs_lock);

        /* Clear invalid bits */
        mode &= 07777;

        mode &= ~umask;
    }

    struct libos_dentry* dir = NULL;

    if (*filename != '/' && (ret = get_dirfd_dentry(dfd, &dir)) < 0)
        return ret;

    hdl = get_new_handle();
    if (!hdl) {
        ret = -ENOMEM;
        goto out;
    }
    ret = open_namei(hdl, dir, filename, flags, mode, NULL);
    if (ret < 0) {
        /* If this was blocking `open` (e.g. on FIFO), it might have returned `-EINTR`. */
        if (ret == -EINTR) {
            ret = -ERESTARTSYS;
        }
        goto out_hdl;
    }

    /* inotify: `IN_OPEN` on the opened object (regular files; the kernel's own event) */
    if (!hdl->is_dir && hdl->dentry && hdl->dentry->inode && S_ISREG(hdl->dentry->inode->type))
        libos_inotify_file_event(hdl->dentry, IN_OPEN);
#ifdef ENABLE_GRAPHICS
graphics_cont:
    if (use_graphics_host) {
        hdl->type = TYPE_HOST;
        ret = set_new_fd_handle_by_fd(retval, hdl, flags & O_CLOEXEC ? FD_CLOEXEC : 0, NULL);
    } else {
        ret = set_new_fd_handle(hdl, flags & O_CLOEXEC ? FD_CLOEXEC : 0, NULL);

        /* Graphics logic to avoid overflow into fd range. */
        if (ret > GRAPHICS_HOST_FD_OFFSET) {
            log_always("error: openat(..) overflowed into host fd range. (ret: %d)\n", ret);
            assert(false);
        }
    }
#else
    ret = set_new_fd_handle(hdl, flags & O_CLOEXEC ? FD_CLOEXEC : 0, NULL);
#endif

out_hdl:
    put_handle(hdl);
out:
    if (dir)
        put_dentry(dir);
    return ret;
}

long libos_syscall_openat(int dfd, const char* filename, int flags, int mode) {
    return do_openat(dfd, filename, flags, mode, true);
}

long libos_openat_internal(int dfd, const char* filename, int flags, int mode) {
    return do_openat(dfd, filename, flags, mode, false);
}

long libos_syscall_close(int fd) {
    long retval;
    struct libos_handle* handle = detach_fd_handle(fd, NULL, NULL);
    if (!handle)
    {
        #ifdef ENABLE_GRAPHICS
            /* An fd with no LibOS handle may belong to the host (graphics); without a host agent
             * (plain Linux PAL, e.g. the whole aarch64 port) the comm window is unmapped and
             * the writes below would fault, so this must be plain EBADF there. */
            if (graphics_redirect_enabled()) {
                long _offset = libos_syscall_acquire_libos_lock();
                comm_page_t* c = comm_page(_offset);
                c->p1 = (uint64_t) fd;
                c->req_bit = CLOSE;

                retval = (long)comm_sync_notify(c);
                libos_syscall_relinquish_libos_lock(_offset);
                return retval;
            }
        #endif
        return -EBADF;
    }
    put_handle(handle);
    return 0;
}

long libos_syscall_close_range(unsigned int first, unsigned int last, unsigned int flags) {
    if (first > last || flags & ~(CLOSE_RANGE_CLOEXEC | CLOSE_RANGE_UNSHARE)) {
        return -EINVAL;
    }
    if (flags & CLOSE_RANGE_UNSHARE) {
        struct libos_thread* thread      = get_cur_thread();
        struct libos_handle_map* new_map = NULL;

        int err = dup_handle_map(&new_map, thread->handle_map);
        if (err < 0) {
            return err;
        }
        set_handle_map(thread, new_map);
        put_handle_map(new_map);
    }
    close_handle_range(first, last, !!(flags & CLOSE_RANGE_CLOEXEC));
    return 0;
}

/* See also `do_getdents`. */
static file_off_t do_lseek_dir(struct libos_handle* hdl, off_t offset, int origin) {
    assert(hdl->is_dir);

    lock(&g_dcache_lock);
    maybe_lock_pos_handle(hdl);
    lock(&hdl->lock);

    file_off_t ret;

    /* Refresh the directory handle, so that after `lseek` the user sees an updated listing. */
    clear_directory_handle(hdl);
    if ((ret = populate_directory_handle(hdl)) < 0)
        goto out;

    struct libos_dir_handle* dirhdl = &hdl->dir_info;

    file_off_t pos;
    ret = generic_seek(hdl->pos, dirhdl->count, offset, origin, &pos);
    if (ret < 0)
        goto out;
    hdl->pos = pos;
    ret = pos;

out:
    unlock(&hdl->lock);
    maybe_unlock_pos_handle(hdl);
    unlock(&g_dcache_lock);
    return ret;
}

/* lseek is simply doing arithmetic on the offset, no PAL call here */
long libos_syscall_lseek(int fd, off_t offset, int origin) {
    long retval;

    #ifdef ENABLE_GRAPHICS
        /* Only with a host agent (VM/TDX PAL): see graphics_redirect_enabled(). Otherwise an
         * invalid fd would be shifted into the host fd range and fault on the unmapped comm
         * window. */
        if (graphics_redirect_enabled() && fd < fs_offset && !get_fd_handle(fd, NULL, NULL))
            fd += fs_offset; // Hacky workaround: shift a handle-less fd into the host fd range (+500); host agent only
        if (graphics_redirect_enabled() && fd>fs_offset) {
            log_debug("lseek (fd: %d offset: %ld origin: %d)\n", fd, offset, origin);
            long _offset = libos_syscall_acquire_libos_lock();
            comm_page_t* c = comm_page(_offset);
            c->p1 = (uint64_t) fd;
            c->p2 = (uint64_t) offset;
            c->p3 = (uint64_t) origin;
            c->req_bit = LSEEK;
            retval = (long)comm_sync_notify(c);
            libos_syscall_relinquish_libos_lock(_offset);
            return retval;
        }
    #endif

    if (origin != SEEK_SET && origin != SEEK_CUR && origin != SEEK_END)
        return -EINVAL;

    struct libos_handle* hdl = get_fd_handle(fd, NULL, NULL);
    if (!hdl)
        return -EBADF;

    off_t ret = 0;
    if (hdl->is_dir) {
        ret = do_lseek_dir(hdl, offset, origin);
        goto out;
    }

    struct libos_fs* fs = hdl->fs;
    assert(fs && fs->fs_ops);

    if (!fs->fs_ops->seek) {
        ret = -ESPIPE;
        goto out;
    }

    ret = fs->fs_ops->seek(hdl, offset, origin);
out:
    put_handle(hdl);
    return ret;
}

long libos_syscall_pread64(int fd, char* buf, size_t count, loff_t offset) {
    if (!is_user_memory_writable(buf, count))
        return -EFAULT;

    if (offset < 0)
        return -EINVAL;

    struct libos_handle* hdl = get_fd_handle(fd, NULL, NULL);
    if (!hdl)
        return -EBADF;

    struct libos_fs* fs = hdl->fs;
    ssize_t ret = -EACCES;

    if (!(hdl->acc_mode & MAY_READ)) {
        ret = -EBADF;
        goto out;
    }
    if (fd_memfd_access_denied(fd, hdl, MAY_READ)) {
        ret = -EBADF;
        goto out;
    }

    if (!fs || !fs->fs_ops)
        goto out;

    if (!fs->fs_ops->seek) {
        ret = -ESPIPE;
        goto out;
    }

    if (!fs->fs_ops->read)
        goto out;

    if (hdl->is_dir) {
        ret = -EISDIR;
        goto out;
    }

    file_off_t pos = offset;
    ret = fs->fs_ops->read(hdl, buf, count, &pos);
out:
    put_handle(hdl);
    return ret;
}

long libos_syscall_pwrite64(int fd, char* buf, size_t count, loff_t offset) {
    if (!is_user_memory_readable(buf, count))
        return -EFAULT;

    if (offset < 0)
        return -EINVAL;

    struct libos_handle* hdl = get_fd_handle(fd, NULL, NULL);
    if (!hdl)
        return -EBADF;

    struct libos_fs* fs = hdl->fs;
    ssize_t ret = -EACCES;

    if (!(hdl->acc_mode & MAY_WRITE)) {
        ret = -EBADF;
        goto out;
    }
    if (fd_memfd_access_denied(fd, hdl, MAY_WRITE)) {
        ret = -EBADF;
        goto out;
    }
    ret = libos_memfd_check_write(hdl, offset, count);
    if (ret < 0) {
        goto out;
    }

    if (!fs || !fs->fs_ops)
        goto out;

    if (!fs->fs_ops->seek) {
        ret = -ESPIPE;
        goto out;
    }

    if (!fs->fs_ops->write)
        goto out;

    if (hdl->is_dir) {
        ret = -EISDIR;
        goto out;
    }

    file_off_t pos = offset;
    ret = handle_append_position(hdl, &pos);
    if (ret == 0)
        ret = handle_fsize_limit(hdl, pos, &count);
    if (ret == 0)
        ret = fs->fs_ops->write(hdl, buf, count, &pos);
out:
    put_handle(hdl);
    return ret;
}

static int get_dirent_type(mode_t type) {
    switch (type) {
        case S_IFLNK:
            return LINUX_DT_LNK;
        case S_IFREG:
            return LINUX_DT_REG;
        case S_IFDIR:
            return LINUX_DT_DIR;
        case S_IFCHR:
            return LINUX_DT_CHR;
        case S_IFBLK:
            return LINUX_DT_BLK;
        case S_IFIFO:
            return LINUX_DT_FIFO;
        case S_IFSOCK:
            return LINUX_DT_SOCK;
        default:
            return LINUX_DT_UNKNOWN;
    }
}

static ssize_t do_getdents(int fd, uint8_t* buf, size_t buf_size, bool is_getdents64) {
    if (!is_user_memory_writable(buf, buf_size))
        return -EFAULT;

    struct libos_handle* hdl = get_fd_handle(fd, NULL, NULL);
    if (!hdl)
        return -EBADF;

    ssize_t ret;

    if (!hdl->is_dir) {
        ret = -ENOTDIR;
        goto out_no_unlock;
    }

    if (!hdl->dentry->inode) {
        ret = -ENOENT;
        goto out_no_unlock;
    }

    lock(&g_dcache_lock);
    maybe_lock_pos_handle(hdl);
    lock(&hdl->lock);

    struct libos_dir_handle* dirhdl = &hdl->dir_info;
    if ((ret = populate_directory_handle(hdl)) < 0)
        goto out;

    size_t buf_pos = 0;
    while ((size_t)hdl->pos < dirhdl->count) {
        struct libos_dentry* dent = dirhdl->dents[hdl->pos];
        assert(dent->inode);

        const char* name;
        size_t name_len;

        if (hdl->pos == 0) {
            name = ".";
            name_len = 1;
        } else if (hdl->pos == 1) {
            name = "..";
            name_len = 2;
        } else {
            name = dent->name;
            name_len = dent->name_len;
        }

        uint64_t d_ino = dentry_ino(dent);
        char d_type = get_dirent_type(dent->inode->type);

        size_t ent_size;

        if (is_getdents64) {
            ent_size = ALIGN_UP(sizeof(struct linux_dirent64) + name_len + 1,
                                alignof(struct linux_dirent64));
            if (buf_pos + ent_size > buf_size)
                break;

            struct linux_dirent64* ent = (struct linux_dirent64*)(buf + buf_pos);
            memset(ent, 0, ent_size); // this ensures `name` will be null-terminated

            ent->d_ino = d_ino;
            ent->d_off = hdl->pos;
            ent->d_reclen = ent_size;
            ent->d_type = d_type;
            memcpy(&ent->d_name, name, name_len);
        } else {
            /* Note that `struct linux_dirent_tail` starts with a zero padding byte, so we don't
             * need to account for extra null byte at the end of `name`. */
            ent_size = ALIGN_UP(
                sizeof(struct linux_dirent) + sizeof(struct linux_dirent_tail) + name_len,
                alignof(struct linux_dirent)
            );
            if (buf_pos + ent_size > buf_size)
                break;

            struct linux_dirent* ent = (struct linux_dirent*)(buf + buf_pos);
            struct linux_dirent_tail* tail =
                (struct linux_dirent_tail*)(buf + buf_pos + ent_size - sizeof(*tail));
            memset(ent, 0, ent_size); // this ensures `name` will be null-terminated

            ent->d_ino = d_ino;
            ent->d_off = hdl->pos;
            ent->d_reclen = ent_size;
            memcpy(&ent->d_name, name, name_len);
            tail->d_type = d_type;
        }

        buf_pos += ent_size;
        hdl->pos++;
    }

    /* Guard against overflow */
    size_t limit = is_getdents64 ? (size_t)LONG_MAX : (size_t)SSIZE_MAX;
    if (buf_pos > limit) {
        ret = -EINVAL;
        goto out;
    }

    /* Return EINVAL if buffer is too small to hold anything */
    if (buf_pos == 0 && (size_t)hdl->pos < dirhdl->count) {
        ret = -EINVAL;
        goto out;
    }

    ret = buf_pos;
out:
    unlock(&hdl->lock);
    maybe_unlock_pos_handle(hdl);
    unlock(&g_dcache_lock);
out_no_unlock:
    put_handle(hdl);
    return ret;
}

static_assert(sizeof(long) <= sizeof(ssize_t),
              "return type of do_getdents() is too small for getdents32");

long libos_syscall_getdents(int fd, struct linux_dirent* buf, unsigned int count) {
    return do_getdents(fd, (uint8_t*)buf, count, /*is_getdents64=*/false);
}

long libos_syscall_getdents64(int fd, struct linux_dirent64* buf, size_t count) {
    long retval;
    #ifdef ENABLE_GRAPHICS
        if(graphics_redirect_enabled() && fd>fs_offset){
            long _offset = libos_syscall_acquire_libos_lock();
            comm_page_t* c = comm_page(_offset);
            c->p1 = (uint64_t) fd;
            c->p2 = (uint64_t) buf;
            c->p3 = (uint64_t) count;
            c->req_bit = GETDENT;
            retval = (long)comm_sync_notify(c);
            libos_syscall_relinquish_libos_lock(_offset);
            return retval;
        }
    #endif
    return do_getdents(fd, (uint8_t*)buf, count, /*is_getdents64=*/true);
}

long libos_syscall_fsync(int fd) {
    struct libos_handle* hdl = get_fd_handle(fd, NULL, NULL);
    if (!hdl)
        return -EBADF;

    int ret;
    struct libos_fs* fs = hdl->fs;

    if (!fs || !fs->fs_ops) {
        ret = -EACCES;
        goto out;
    }

    if (hdl->is_dir) {
        /* FS subsystem doesn't do anything meaningful with dirs, so flushing a dir is a no-op */
        ret = 0;
        goto out;
    }

    if (!fs->fs_ops->flush) {
        ret = -EINVAL; /* Linux kernel returns EINVAL on special files (not EROFS) */
        goto out;
    }

    ret = fs->fs_ops->flush(hdl);
out:
    put_handle(hdl);
    return ret;
}

long libos_syscall_fdatasync(int fd) {
    /* assume fsync() >> fdatasync(); no app should depend on only syncing data for correctness */
    return libos_syscall_fsync(fd);
}

long libos_syscall_sync(void) {
    /* Filesystem writes are issued synchronously by the PAL callbacks. */
    return 0;
}

long libos_syscall_syncfs(int fd) {
    struct libos_handle* hdl = get_fd_handle(fd, NULL, NULL);
    if (!hdl)
        return -EBADF;
    put_handle(hdl);
    return libos_syscall_fsync(fd);
}

long libos_syscall_sync_file_range(int fd, loff_t offset, loff_t nbytes, unsigned int flags) {
    const unsigned int supported = 1U | 2U | 4U;
    if (offset < 0 || nbytes < 0 || (flags & ~supported))
        return -EINVAL;
    struct libos_handle* hdl = get_fd_handle(fd, NULL, NULL);
    if (!hdl)
        return -EBADF;
    if (hdl->inode && hdl->inode->type != S_IFREG && hdl->inode->type != S_IFDIR) {
        put_handle(hdl);
        return -ESPIPE;
    }
    bool can_flush = hdl->fs && hdl->fs->fs_ops && hdl->fs->fs_ops->flush;
    put_handle(hdl);
    if ((flags & 2U) && can_flush)
        return libos_syscall_fsync(fd);
    return 0;
}

long libos_syscall_readahead(int fd, loff_t offset, size_t count) {
    __UNUSED(count);
    if (offset < 0)
        return -EINVAL;
    struct libos_handle* hdl = get_fd_handle(fd, NULL, NULL);
    if (!hdl)
        return -EBADF;
    long ret = 0;
    if (!(hdl->acc_mode & MAY_READ))
        ret = -EBADF;
    else if (hdl->is_dir)
        ret = -EISDIR;
    else if (!hdl->fs || !hdl->fs->fs_ops || !hdl->fs->fs_ops->seek)
        ret = -EINVAL;
    put_handle(hdl);
    return ret;
}

long libos_syscall_truncate(const char* path, loff_t length) {
    if (length < 0)
        return -EINVAL;

    int ret;

    if (!is_user_string_readable(path))
        return -EFAULT;

    struct libos_handle* hdl = get_new_handle();
    if (!hdl)
        return -ENOMEM;

    ret = open_namei(hdl, /*start=*/NULL, path, LOOKUP_FOLLOW, O_WRONLY, /*found=*/NULL);
    if (ret < 0)
        goto out;

    struct libos_fs* fs = hdl->fs;

    if (!fs->fs_ops->truncate) {
        ret = -EROFS;
        goto out;
    }

    /* growing past RLIMIT_FSIZE: EFBIG and SIGXFSZ (LTP truncate03) */
    size_t over = 1;
    if (length > 0 && (ret = handle_fsize_limit(hdl, length - 1, &over)) < 0)
        goto out;

    ret = fs->fs_ops->truncate(hdl, length);
out:
    put_handle(hdl);
    return ret;
}

long libos_syscall_ftruncate(int fd, loff_t length) {
    if (length < 0)
        return -EINVAL;

    struct libos_handle* hdl = get_fd_handle(fd, NULL, NULL);
    if (!hdl)
        return -EBADF;

#ifdef ENABLE_GRAPHICS
    if (hdl->type == TYPE_HOST) {
        long offset = libos_syscall_acquire_libos_lock();
        comm_page_t* c = comm_page(offset);
        c->p1 = fd;
        c->p2 = length;
        c->req_bit = HOST_FTRUNCATE;
        put_handle(hdl);

        long ret = (int64_t)comm_sync_notify(c);
        libos_syscall_relinquish_libos_lock(offset);
        return ret;
    }
#endif

    struct libos_fs* fs = hdl->fs;
    int ret;

    if (!(hdl->acc_mode & MAY_WRITE)) {
        /* Note that we return EINVAL here, not EBADF like in `read`/`write`. This is what Linux
         * does. */
        ret = -EINVAL;
        goto out;
    }

    if (!fs || !fs->fs_ops) {
        ret = -EINVAL;
        goto out;
    }

    if (hdl->is_dir || !fs->fs_ops->truncate) {
        ret = -EINVAL;
        goto out;
    }

    size_t over = 1;
    if (length > 0 && (ret = handle_fsize_limit(hdl, length - 1, &over)) < 0)
        goto out;
    if (hdl->is_memfd && hdl->inode) {
        if ((length < hdl->inode->size && (hdl->memfd_seals & LIBOS_F_SEAL_SHRINK)) ||
            (length > hdl->inode->size && (hdl->memfd_seals & LIBOS_F_SEAL_GROW))) {
            ret = -EPERM;
            goto out;
        }
    }

    ret = fs->fs_ops->truncate(hdl, length);
out:
    put_handle(hdl);
    return ret;
}

long libos_syscall_fallocate(int fd, int mode, loff_t offset, loff_t len) {
    int ret;
    if (offset < 0) {
        return -EINVAL;
    }
    if (len <= 0) {
        return -EINVAL;
    }
    const int punch_hole_mode = FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE;
    if (mode && mode != punch_hole_mode) {
        log_warning("fallocate mode 0x%x is not supported", mode);
        return -ENOSYS;
    }

    struct libos_handle* handle = get_fd_handle(fd, NULL, NULL);
    if (!handle) {
        return -EBADF;
    }
    if (handle->type == TYPE_PIPE) {
        ret = -ESPIPE;
        goto out;
    }
    if (!handle->inode || handle->inode->type != S_IFREG) {
        ret = -ENODEV;
        goto out;
    }

    if (!(handle->acc_mode & MAY_WRITE)) {
        ret = -EBADF;
        goto out;
    }

    struct libos_fs* fs = handle->fs;
    if (!fs || !fs->fs_ops) {
        ret = -EINVAL;
        goto out;
    }
    if (!fs->fs_ops->truncate) {
        ret = -EOPNOTSUPP;
        goto out;
    }

    loff_t end;
    if (__builtin_add_overflow(offset, len, &end)) {
        ret = -EFBIG;
        goto out;
    }

    file_off_t current_size;
    lock(&handle->inode->lock);
    current_size = handle->inode->size;
    unlock(&handle->inode->lock);
    if (handle->is_memfd) {
        if ((handle->memfd_seals & LIBOS_F_SEAL_WRITE) &&
            mode == punch_hole_mode) {
            ret = -EPERM;
            goto out;
        }
        if ((handle->memfd_seals & LIBOS_F_SEAL_GROW) && mode == 0 && end > current_size) {
            ret = -EPERM;
            goto out;
        }
    }

    if (mode == punch_hole_mode) {
        static const char zeros[4096];
        lock(&handle->inode->lock);
        file_off_t size = handle->inode->size;
        unlock(&handle->inode->lock);
        file_off_t punch_end = end < size ? end : size;
        loff_t pos = offset;
        while (pos < punch_end) {
            loff_t remaining = punch_end - pos;
            size_t count = remaining < (loff_t)sizeof(zeros) ? (size_t)remaining : sizeof(zeros);
            ssize_t written = fs->fs_ops->write(handle, zeros, count, &pos);
            if (written < 0) {
                ret = (int)written;
                break;
            }
            if (!written) {
                ret = -EIO;
                break;
            }
        }
        if (pos >= punch_end)
            ret = 0;
        goto out;
    }

    /* Simple implementation: extend the file if required, otherwise act as a no-op.
     * WARNING: if two threads try doing `fallocate` at the same time with 2 different sizes (and
     * both bigger than the current size) or one does `fallocate` and the other tries writing to
     * the end of the file, there is a possibility of a race which would actually truncate the file.
     * Hopefully no sane application does that. */
    lock(&handle->inode->lock);
    file_off_t size = handle->inode->size;
    unlock(&handle->inode->lock);
    if (end > size) {
        ret = fs->fs_ops->truncate(handle, end);
    } else {
        ret = 0;
    }

out:
    put_handle(handle);
    return ret;
}

long libos_syscall_fadvise64(int fd, loff_t offset, size_t len, int advice) {
    __UNUSED(offset);
    __UNUSED(len);
    int ret;

    switch (advice) {
        case POSIX_FADV_NORMAL:
        case POSIX_FADV_RANDOM:
        case POSIX_FADV_SEQUENTIAL:
        case POSIX_FADV_WILLNEED:
        case POSIX_FADV_NOREUSE:
        case POSIX_FADV_DONTNEED:
            break;
        default:
            return -EINVAL;
    }

    struct libos_handle* handle = get_fd_handle(fd, NULL, NULL);
    if (!handle) {
        return -EBADF;
    }

    if (handle->type == TYPE_PIPE) {
        ret = -ESPIPE;
        goto out;
    }

    /* currently just a no-op */
    ret = 0;

out:
    put_handle(handle);
    return ret;
}
