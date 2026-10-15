/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2014 Stony Brook University
 * Copyright (C) 2021 Intel Corporation
 *                    Paweł Marczewski <pawel@invisiblethingslab.com>
 */

/*
 * Implementation of system calls "fcntl" and "flock".
 *
 * The "fcntl" syscall supports:
 *
 * - F_DUPFD, F_DUPFD_CLOEXEC (duplicate a file descriptor)
 * - F_GETFD, F_SETFD (file descriptor flags)
 * - F_GETFL, F_SETFL (file status flags)
 * - F_SETLK, F_SETLKW, F_GETLK (POSIX advisory locks)
 * - F_SETOWN, F_GETOWN (file descriptor owner): recorded, not used for signal routing
 * - F_SETSIG, F_GETSIG (the signal of F_NOTIFY notifications)
 * - F_SETLEASE, F_GETLEASE (leases: process-local bookkeeping, no lease breaking)
 * - F_NOTIFY (dnotify, from the same events as inotify, see `libos_inotify.c`)
 * - F_GETPIPE_SZ, F_SETPIPE_SZ (the capacity is recorded, not enforced by the PAL pipe)
 */

#include "libos_fs.h"
#include "libos_graphics.h"
#include "libos_fs_lock.h"
#include "libos_handle.h"
#include "libos_inotify.h"
#include "libos_internal.h"
#include "libos_lock.h"
#include "libos_process.h"
#include "libos_table.h"
#include "libos_thread.h"
#include "libos_vma.h"
#include "linux_abi/errors.h"
#include "linux_abi/fs.h"
#include "stat.h"
#include "toml_utils.h"
#define FCNTL_SETFL_MASK (O_APPEND | O_DIRECT | O_NOATIME | O_NONBLOCK)
#define F_ADD_SEALS 1033
#define F_GET_SEALS 1034
#define F_SEAL_SEAL         LIBOS_F_SEAL_SEAL
#define F_SEAL_SHRINK       LIBOS_F_SEAL_SHRINK
#define F_SEAL_GROW         LIBOS_F_SEAL_GROW
#define F_SEAL_WRITE        LIBOS_F_SEAL_WRITE
#define F_SEAL_FUTURE_WRITE LIBOS_F_SEAL_FUTURE_WRITE
#define F_SEAL_EXEC         LIBOS_F_SEAL_EXEC
#define F_SEAL_MASK (F_SEAL_SEAL | F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE | \
                     F_SEAL_FUTURE_WRITE | F_SEAL_EXEC)
#ifndef F_GETPIPE_SZ
#define F_GETPIPE_SZ 1032
#endif
#ifndef F_SETPIPE_SZ
#define F_SETPIPE_SZ 1031
#endif
#ifndef F_SETLEASE
#define F_SETLEASE 1024
#endif
#ifndef F_GETLEASE
#define F_GETLEASE 1025
#endif
#ifndef F_NOTIFY
#define F_NOTIFY 1026
#endif
#ifndef F_SETSIG
#define F_SETSIG 10
#endif
#ifndef F_GETSIG
#define F_GETSIG 11
#endif
#ifndef F_SETOWN_EX
#define F_SETOWN_EX 15
#define F_GETOWN_EX 16
#endif
#define LIBOS_F_OWNER_TID  0
#define LIBOS_F_OWNER_PID  1
#define LIBOS_F_OWNER_PGRP 2
struct libos_f_owner_ex {
    int type;
    int pid;
};
#define LIBOS_CAP_SYS_RESOURCE 24
#define LIBOS_CAP_LEASE        28
/* the kernel defaults of /proc/sys/fs/pipe-max-size and a new pipe's capacity */
#define PIPE_MAX_SIZE     (1024 * 1024)
#define PIPE_DEFAULT_SIZE 65536

/* How many open file descriptions of this process refer to `inode`, and how many of them are open
 * for writing / reading, `self` excluded (the kernel's `i_writecount`/`i_readcount` view for the
 * lease conflict check; process-local, as the rest of the lease bookkeeping). */
static void count_inode_opens(struct libos_inode* inode, struct libos_handle* self,
                              size_t* out_writers, size_t* out_readers) {
    *out_writers = 0;
    *out_readers = 0;
    struct libos_handle_map* map = get_thread_handle_map(NULL);
    if (!map)
        return;
    struct libos_handle* seen[64];
    size_t nseen = 0;
    rwlock_read_lock(&map->lock);
    for (uint32_t fd = 0; map->fd_top != FD_NULL && fd <= map->fd_top; fd++) {
        if (!HANDLE_ALLOCATED(map->map[fd]))
            continue;
        struct libos_handle* h = map->map[fd]->handle;
        if (!h || h == self || h->inode != inode)
            continue;
        bool dup = false; /* dup'ed descriptors share one open file description */
        for (size_t i = 0; i < nseen; i++)
            dup = dup || seen[i] == h;
        if (dup)
            continue;
        if (nseen < ARRAY_SIZE(seen))
            seen[nseen++] = h;
        if (h->acc_mode & MAY_WRITE)
            (*out_writers)++;
        else
            (*out_readers)++;
    }
    rwlock_read_unlock(&map->lock);
}

/* F_SETLEASE: the kernel's `generic_setlease()` checks, without lease breaking (a conflicting
 * open later succeeds instead of breaking the lease; see the libos-syscalls TODO.md). */
static int set_lease(struct libos_handle* hdl, unsigned long arg) {
    if (arg != F_RDLCK && arg != F_WRLCK && arg != F_UNLCK)
        return -EINVAL;
    if (!hdl->inode || hdl->inode->type != S_IFREG)
        return -EINVAL;
    if (arg == F_UNLCK) {
        lock(&hdl->lock);
        hdl->has_lease = false;
        unlock(&hdl->lock);
        return 0;
    }
    /* only the file's owner, or with CAP_LEASE */
    if ((unsigned int)libos_syscall_getuid() != hdl->inode->uid && !libos_capable(LIBOS_CAP_LEASE))
        return -EACCES;
    size_t writers, readers;
    count_inode_opens(hdl->inode, hdl, &writers, &readers);
    bool self_writes = hdl->acc_mode & MAY_WRITE;
    if (arg == F_RDLCK) {
        /* a read lease conflicts with any writer, this description included */
        if (writers || self_writes)
            return -EAGAIN;
    } else {
        /* a write lease needs this description to be the only one */
        if (writers || readers)
            return -EAGAIN;
    }
    lock(&hdl->lock);
    hdl->has_lease = true;
    hdl->lease_type = (int)arg;
    unlock(&hdl->lock);
    return 0;
}

/* F_SETPIPE_SZ: the kernel's checks and rounding (`pipe_set_size()`); the size is recorded on both
 * ends and reported by F_GETPIPE_SZ, but the PAL pipe does not enforce it. */
static int set_pipe_size(struct libos_handle* hdl, unsigned long arg) {
    if (hdl->type != TYPE_PIPE)
        return -EBADF;
    if (arg > (1UL << 31))
        return -EINVAL;
    size_t size = arg < 4096 ? 4096 : arg;
    /* rounded up to a power-of-two number of pages, as the kernel does */
    size_t rounded = 4096;
    while (rounded < size)
        rounded *= 2;
    if (rounded > PIPE_MAX_SIZE && !libos_capable(LIBOS_CAP_SYS_RESOURCE))
        return -EPERM;

    /* EBUSY if the pipe holds more than the new size: ask the read end how much is queued */
    struct libos_handle* rd = hdl;
    struct libos_handle* peer = NULL;
    if (!(hdl->acc_mode & MAY_READ) && hdl->info.pipe.peer_fd >= 0) {
        peer = get_fd_handle(hdl->info.pipe.peer_fd, NULL, NULL);
        if (peer && peer->type == TYPE_PIPE && peer->pal_handle)
            rd = peer;
    }
    if (rd->pal_handle) {
        PAL_STREAM_ATTR attr;
        if (PalStreamAttributesQueryByHandle(rd->pal_handle, &attr) == 0 &&
                attr.pending_size > rounded) {
            if (peer)
                put_handle(peer);
            return -EBUSY;
        }
    }
    hdl->info.pipe.size = (uint32_t)rounded;
    if (peer && peer != hdl && peer->type == TYPE_PIPE)
        peer->info.pipe.size = (uint32_t)rounded;
    if (peer)
        put_handle(peer);
    return (int)rounded;
}

static int memfd_has_writable_shared_mapping(struct libos_handle* handle) {
    struct libos_vma_info* vmas = NULL;
    size_t count = 0;
    int ret = dump_all_vmas(/*include_unmapped=*/false, &vmas, &count);
    if (ret < 0)
        return ret;

    bool found = false;
    for (size_t i = 0; i < count; i++) {
        if (vmas[i].file == handle && (vmas[i].flags & MAP_SHARED) &&
            (vmas[i].prot & PROT_WRITE)) {
            found = true;
            break;
        }
    }
    free_vma_info_array(vmas, count);
    return found;
}

static int generic_set_flags(struct libos_handle* handle, unsigned int flags, unsigned int mask) {
    /* TODO: DOES THIS WORK LOL
     * The old version of this code did this, but this seem to be incorrect. If a handle type allows
     * for setting some flags without actually doing anything with them immediately, it should have
     * a `setflags` callback implementation. */
    lock(&handle->lock);
    handle->flags = (handle->flags & ~mask) | flags;
    unlock(&handle->lock);
    return 0;
}

static int set_handle_flags(struct libos_handle* handle, unsigned int flags, unsigned int mask) {
    flags &= mask;
    if (handle->fs && handle->fs->fs_ops && handle->fs->fs_ops->setflags) {
        return handle->fs->fs_ops->setflags(handle, flags, mask);
    }
    return generic_set_flags(handle, flags, mask);
}

int set_handle_nonblocking(struct libos_handle* handle, bool on) {
    return set_handle_flags(handle, on ? O_NONBLOCK : 0, O_NONBLOCK);
}

/*
 * Convert user-mode `struct flock` into our `struct libos_file_lock` (for POSIX locks only). This
 * mostly means converting the position parameters (l_whence, l_start, l_len) to an absolute
 * inclusive range [start .. end]. See `man fcntl` for details.
 *
 * We need to return -EINVAL for underflow (positions before start of file), and -EOVERFLOW for
 * positive overflow.
 */
static int flock_to_file_lock(struct flock* fl, struct libos_handle* hdl,
                              struct libos_file_lock* file_lock) {
    if (!(fl->l_type == F_RDLCK || fl->l_type == F_WRLCK || fl->l_type == F_UNLCK))
        return -EINVAL;

    int ret;

    struct libos_fs* fs = hdl->fs;
    assert(fs && fs->fs_ops);

    /* Compute the origin based on `l_start` and `l_whence`. Note that we cannot directly call
     * `seek(hdl, l_start, l_whence)`, because that would modify the handle position. Only
     * retrieving the current position (by calling `seek(hdl, 0, SEEK_CUR)`) is safe. */
    uint64_t origin;
    switch (fl->l_whence) {
        case SEEK_SET:
            origin = 0;
            break;
        case SEEK_CUR: {
            if (!fs->fs_ops->seek)
                return -EINVAL;

            file_off_t pos = fs->fs_ops->seek(hdl, 0, SEEK_CUR);
            if (pos < 0)
                return pos;
            origin = pos;
            break;
        }
        case SEEK_END: {
            if (!fs->fs_ops->hstat)
                return -EINVAL;

            struct stat stat;
            ret = fs->fs_ops->hstat(hdl, &stat);
            if (ret < 0)
                return ret;
            assert(stat.st_size >= 0);
            origin = stat.st_size;
            break;
        }
        default:
            return -EINVAL;
    }

    if (__builtin_add_overflow(origin, fl->l_start, &origin)) {
        return fl->l_start > 0 ? -EOVERFLOW : -EINVAL;
    }

    uint64_t start, end;
    if (fl->l_len > 0) {
        /* len > 0: the range is [origin .. origin + len - 1] */
        start = origin;
        if (__builtin_add_overflow(origin, fl->l_len - 1, &end))
            return -EOVERFLOW;
    } else if (fl->l_len < 0) {
        /* len < 0: the range is [origin + len .. origin - 1] */
        if (__builtin_add_overflow(origin, fl->l_len, &start))
            return -EINVAL;
        if (__builtin_add_overflow(origin, -1, &end))
            return -EINVAL;
    } else {
        /* len == 0: the range is [origin .. EOF] */
        start = origin;
        end = FS_LOCK_EOF;
    }

    file_lock->family = FILE_LOCK_POSIX;
    file_lock->type = fl->l_type;
    file_lock->start = start;
    file_lock->end = end;
    file_lock->pid = g_process.pid;
    file_lock->handle_id = 0; /* unused in POSIX (fcntl) locks, unset for sanity */
    return 0;
}

long libos_syscall_fcntl(int fd, int cmd, unsigned long arg) {
    long retval;
    (void)retval; /* only assigned on the ENABLE_GRAPHICS comm-window redirect path */
    struct libos_handle* hdl;
    int ret;
    int flags;

    struct libos_handle_map* handle_map = get_thread_handle_map(NULL);
    assert(handle_map);

    hdl = get_fd_handle(fd, &flags, handle_map);
    if (!hdl)
        return -EBADF;

    #ifdef ENABLE_GRAPHICS
        if(hdl->type == TYPE_HOST)
        {
            log_debug("redirecting fcntl(%d, %d, %lu) to host", fd, cmd, arg);
            long _offset = libos_syscall_acquire_libos_lock();
            comm_page_t* c = comm_page(_offset);
            c->p1 = (uint64_t) fd;
            c->p2 = (uint64_t) cmd;
            c->p3 = (uint64_t) arg;
            c->req_bit = FCNTL;

            retval = comm_sync_notify(c);
            libos_syscall_relinquish_libos_lock(_offset);
        }
    #endif

    switch (cmd) {
        /* See `man fcntl` for the expected semantics of these commands. */

        /* F_DUPFD (int) */
        case F_DUPFD: {
            ret = set_new_fd_handle_above_fd(arg, hdl, flags, handle_map);
            break;
        }

        /* F_DUPFD_CLOEXEC (int) */
        case F_DUPFD_CLOEXEC: {
            flags |= FD_CLOEXEC;

            /* Same semantics as F_DUPFD: `arg` is the *minimum* fd number; the kernel picks
             * the lowest free fd >= arg. (Assigning exactly `arg` fails with EBADF whenever
             * `arg` is occupied, which is common: callers pass small minimums, e.g. tokio's
             * epoll probe `fcntl(epfd, F_DUPFD_CLOEXEC, epfd)`.) The TYPE_HOST comm-window
             * handle was redirected to the host before the switch above. */
#ifdef ENABLE_GRAPHICS
            if (hdl->type == TYPE_HOST) {
                /* The host duplicated its descriptor and returned the new one in the host fd
                 * range; a TYPE_HOST handle is identified by that number, so the guest fd has to
                 * be exactly it. */
                ret = retval < 0 ? (int)retval : set_new_fd_handle_by_fd(retval, hdl, flags,
                                                                         handle_map);
                break;
            }
#endif
            ret = set_new_fd_handle_above_fd(arg, hdl, flags, handle_map);
            break;
        }

        /* F_GETFD (int) */
        case F_GETFD:
            ret = flags & FD_CLOEXEC;
            break;

        /* F_SETFD (int) */
        case F_SETFD:
            rwlock_write_lock(&handle_map->lock);
            if (HANDLE_ALLOCATED(handle_map->map[fd]))
                handle_map->map[fd]->flags = arg & FD_CLOEXEC;
            rwlock_write_unlock(&handle_map->lock);
            ret = 0;
            break;

        /* F_GETFL (void) */
        case F_GETFL:
            lock(&hdl->lock);
            flags = hdl->flags;
            unlock(&hdl->lock);
            ret = flags;
            break;

        case F_GETPIPE_SZ:
            /* The recorded nominal capacity. The PAL stream API does not enforce a bounded FIFO
             * ring; vmsplice observes that limitation. */
            if (hdl->type != TYPE_PIPE)
                ret = -EBADF;
            else
                ret = hdl->info.pipe.size ? (int)hdl->info.pipe.size : PIPE_DEFAULT_SIZE;
            break;

        case F_SETPIPE_SZ:
            ret = set_pipe_size(hdl, arg);
            break;

        case F_SETLEASE:
            ret = set_lease(hdl, arg);
            break;

        case F_GETLEASE:
            lock(&hdl->lock);
            ret = hdl->has_lease ? hdl->lease_type : F_UNLCK;
            unlock(&hdl->lock);
            break;

        /* F_SETSIG (int): 0 means SIGIO, otherwise the signal F_NOTIFY delivers (with si_fd) */
        case F_SETSIG:
            if (arg > SIGS_CNT) {
                ret = -EINVAL;
                break;
            }
            lock(&hdl->lock);
            hdl->fown_signal = (int)arg;
            unlock(&hdl->lock);
            ret = 0;
            break;

        case F_GETSIG:
            lock(&hdl->lock);
            ret = hdl->fown_signal;
            unlock(&hdl->lock);
            break;

        case F_NOTIFY:
            ret = libos_dnotify_set(hdl, fd, arg);
            break;

        /* F_SETFL (int) */
        case F_SETFL:
            ret = set_handle_flags(hdl, arg, FCNTL_SETFL_MASK);
            break;

        case F_GET_SEALS:
            if (!hdl->is_memfd) {
                ret = -EINVAL;
                break;
            }
            lock(&hdl->lock);
            ret = hdl->memfd_seals;
            unlock(&hdl->lock);
            break;

        case F_ADD_SEALS:
            if (!hdl->is_memfd || (arg & ~F_SEAL_MASK)) {
                ret = -EINVAL;
                break;
            }
            if (!(get_fd_acc_mode(fd, handle_map) & MAY_WRITE)) {
                ret = -EPERM;
                break;
            }
            lock(&hdl->lock);
            if (hdl->memfd_seals & F_SEAL_SEAL)
                ret = -EPERM;
            else if ((arg & F_SEAL_WRITE) && !(hdl->memfd_seals & F_SEAL_WRITE) &&
                     (ret = memfd_has_writable_shared_mapping(hdl)) != 0)
                ret = ret < 0 ? ret : -EBUSY;
            else {
                hdl->memfd_seals |= arg;
                ret = 0;
            }
            unlock(&hdl->lock);
            break;

        /* F_SETLK, F_SETLKW (struct flock*): see `libos_fs_lock.h` for caveats */
        case F_SETLK:
        case F_SETLKW: {
            struct flock* fl = (struct flock*)arg;
            if (!is_user_memory_readable(fl, sizeof(*fl))) {
                ret = -EFAULT;
                break;
            }

            if (!hdl->dentry) {
                /* TODO: Linux allows locks on pipes etc. Our locks work only for "normal" files
                 * that have a dentry. */
                ret = -EINVAL;
                break;
            }

            if (fl->l_type == F_RDLCK && !(hdl->acc_mode & MAY_READ)) {
                ret = -EINVAL;
                break;
            }

            if (fl->l_type == F_WRLCK && !(hdl->acc_mode & MAY_WRITE)) {
                ret = -EINVAL;
                break;
            }

            struct libos_file_lock file_lock;
            ret = flock_to_file_lock(fl, hdl, &file_lock);
            if (ret < 0)
                break;

            ret = file_lock_set(hdl->dentry, &file_lock, /*wait=*/cmd == F_SETLKW);
            break;
        }

        /* F_GETLK (struct flock*): see `libos_fs_lock.h` for caveats */
        case F_GETLK: {
            struct flock* fl = (struct flock*)arg;
            if (!is_user_memory_readable(fl, sizeof(*fl))
                    || !is_user_memory_writable(fl, sizeof(*fl))) {
                ret = -EFAULT;
                break;
            }

            if (!hdl->dentry) {
                ret = -EINVAL;
                break;
            }

            struct libos_file_lock file_lock;
            ret = flock_to_file_lock(fl, hdl, &file_lock);
            if (ret < 0)
                break;

            if (file_lock.type == F_UNLCK) {
                ret = -EINVAL;
                break;
            }

            struct libos_file_lock file_lock2;
            ret = file_lock_get(hdl->dentry, &file_lock, &file_lock2);
            if (ret < 0)
                break;

            fl->l_type = file_lock2.type;
            if (file_lock2.type != F_UNLCK) {
                fl->l_whence = SEEK_SET;
                fl->l_start = file_lock2.start;
                if (file_lock2.end == FS_LOCK_EOF) {
                    /* range until EOF is encoded as len == 0 */
                    fl->l_len = 0;
                } else {
                    fl->l_len = file_lock2.end - file_lock2.start + 1;
                }
                fl->l_pid = file_lock2.pid;
            }
            ret = 0;
            break;
        }

        /* F_SETOWN (int): recorded and reported by F_GETOWN; signals of F_NOTIFY go to this
         * process whatever the owner (one process in the VM, process-local state in direct) */
        case F_SETOWN:
            lock(&hdl->lock);
            hdl->fown_pid = (int)arg;
            unlock(&hdl->lock);
            ret = 0;
            break;

        case F_GETOWN:
            lock(&hdl->lock);
            ret = hdl->fown_pid;
            unlock(&hdl->lock);
            break;

        /* F_SETOWN_EX/F_GETOWN_EX (struct f_owner_ex*): glibc implements F_GETOWN with the
         * latter; a process group is kept as a negative pid, as F_SETOWN does */
        case F_SETOWN_EX: {
            struct libos_f_owner_ex* owner = (struct libos_f_owner_ex*)arg;
            if (!is_user_memory_readable(owner, sizeof(*owner))) {
                ret = -EFAULT;
                break;
            }
            if (owner->type != LIBOS_F_OWNER_TID && owner->type != LIBOS_F_OWNER_PID &&
                    owner->type != LIBOS_F_OWNER_PGRP) {
                ret = -EINVAL;
                break;
            }
            lock(&hdl->lock);
            hdl->fown_pid = owner->type == LIBOS_F_OWNER_PGRP ? -owner->pid : owner->pid;
            unlock(&hdl->lock);
            ret = 0;
            break;
        }

        case F_GETOWN_EX: {
            struct libos_f_owner_ex* owner = (struct libos_f_owner_ex*)arg;
            if (!is_user_memory_writable(owner, sizeof(*owner))) {
                ret = -EFAULT;
                break;
            }
            lock(&hdl->lock);
            int pid = hdl->fown_pid;
            unlock(&hdl->lock);
            owner->type = pid < 0 ? LIBOS_F_OWNER_PGRP : LIBOS_F_OWNER_PID;
            owner->pid = pid < 0 ? -pid : pid;
            ret = 0;
            break;
        }

        default:
            ret = -EINVAL;
            break;
    }

    put_handle(hdl);
    return ret;
}

long libos_syscall_flock(unsigned int fd, unsigned int cmd) {
    int ret;

    /* support for LOCK_{MAND,READ,WRITE,RW} was removed from Linux; it simply ignores LOCK_MAND
     * requests so we do the same */
    if (cmd & LOCK_MAND) {
        log_warning("flock requests with LOCK_MAND are ignored");
        return 0;
    }

    /* TODO: temporary measure, remove it once flock implementation is thoroughly validated and
     * works on multi-process apps; see comments at `created_by_process` in `libos_handle.h` */
    assert(g_manifest_root);
    bool enable_flock;
    ret = toml_bool_in(g_manifest_root, "sys.experimental__enable_flock", /*defaultval=*/false,
                       &enable_flock);
    if (ret < 0) {
        log_error("Cannot parse 'sys.experimental__enable_flock' (the value must be `true` or "
                  "`false`)");
        return -ENOSYS;
    }
    if (!enable_flock) {
        /* flock is not explicitly allowed in manifest */
        if (FIRST_TIME()) {
            log_warning("The app tried to use flock, but it's turned off "
                        "(sys.experimental__enable_flock = false)");
        }

        return -ENOSYS;
    }


    struct libos_handle* hdl = get_fd_handle(fd, /*fd_flags=*/NULL, /*map=*/NULL);
    if (!hdl)
        return -EBADF;

#ifdef ENABLE_GRAPHICS
    if (hdl->type == TYPE_HOST || hdl->type == TYPE_HOST_IDENTITY) {
        long offset = libos_syscall_acquire_libos_lock();
        comm_page_t* c = comm_page(offset);
        c->p1 = fd;
        c->p2 = cmd;
        c->req_bit = HOST_FLOCK;

        ret = (long)comm_sync_notify(c);
        libos_syscall_relinquish_libos_lock(offset);
        goto out;
    }
#endif

    if (!hdl->dentry) {
        /* File-lock bookkeeping currently requires a filesystem dentry. This matches the
         * existing fcntl(F_SETLK) behavior for device, pipe, and other synthetic handles. */
        ret = -EINVAL;
        goto out;
    }

    int lock_type;
    switch (cmd & ~LOCK_NB) {
        case LOCK_EX:
            lock_type = F_WRLCK;
            break;
        case LOCK_SH:
            lock_type = F_RDLCK;
            break;
        case LOCK_UN:
            lock_type = F_UNLCK;
            break;
        default:
            ret = -EINVAL;
            goto out;
    }

    struct libos_file_lock file_lock = {
        .family = FILE_LOCK_FLOCK,
        .type = lock_type,
        .handle_id = hdl->id,
    };
    ret = file_lock_set(hdl->dentry, &file_lock, !(cmd & LOCK_NB));
out:
    put_handle(hdl);
    return ret;
}
