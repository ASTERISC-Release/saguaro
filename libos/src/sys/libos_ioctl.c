/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2014 Stony Brook University */

/*
 * Implementation of system call "ioctl".
 */

#include "libos_handle.h"
#include "libos_internal.h"
#include "libos_process.h"
#include "libos_rwlock.h"
#include "libos_signal.h"
#include "libos_table.h"
#include "linux_abi/errors.h"
#include "linux_abi/ioctl.h"
#include "libos_device_ioctl.h"
#include "libos_graphics.h"
#include "pal.h"
#include "stat.h"

static void signal_io(IDTYPE caller, void* arg) {
    __UNUSED(caller);
    __UNUSED(arg);
    /* TODO: fill these values e.g. by getting the handle in arg; this is completely unusable now */
    siginfo_t info = {
        .si_signo = SIGIO,
        .si_code = SI_SIGIO,
        .si_band = 0,
        .si_fd = 0,
    };
    if (kill_current_proc(&info) < 0) {
        log_warning("signal_io: failed to deliver a signal");
    }
}

#ifdef ENABLE_GRAPHICS
/*
 * A device ioctl forwarded to the host listener (gramine-vm graphics redirection). Pointers inside
 * the argument need nothing (guest RAM is mapped at the same addresses in the listener), but the
 * fields listed in libos_device_ioctl.h do:
 *  - FD_IN: a LibOS fd of a forwarded host object (TYPE_HOST, installed at host fd +
 *    GRAPHICS_HOST_FD_OFFSET) is replaced by its host fd for the call; any other value is passed
 *    unchanged (the graphics preload library hands raw host fds to Mesa itself, see egl.c);
 *  - FD_OUT: the host fd the kernel wrote is installed as a new TYPE_HOST handle, and its LibOS fd
 *    is written back;
 *  - TIME_ABS: an absolute CLOCK_MONOTONIC deadline on Gramine's clock is made relative to "now",
 *    and the listener adds its own CLOCK_MONOTONIC (the guest cannot read the host's clock); the
 *    comm page's p4 names the field (offset + 1) and p5 carries GRAPHICS_IOCTL_TIME_REL_MAGIC.
 * Input fields are restored after the call, so the application sees its own values.
 */
static int install_host_fd(int host_fd, bool cloexec) {
    int fd = host_fd + (int)GRAPHICS_HOST_FD_OFFSET;
    int ret = graphics_drop_stale_host_handle(fd);
    if (ret < 0)
        return ret;
    struct libos_handle* hdl = get_new_handle();
    if (!hdl)
        return -ENOMEM;
    hdl->type = TYPE_HOST;
    ret = set_new_fd_handle_by_fd(fd, hdl, cloexec ? FD_CLOEXEC : 0, NULL);
    put_handle(hdl);
    return ret;
}

static void close_host_fd(int host_fd) {
    long _offset = libos_syscall_acquire_libos_lock();
    comm_page_t* c = comm_page(_offset);
    c->p1 = (uint64_t)host_fd + GRAPHICS_HOST_FD_OFFSET;
    c->req_bit = CLOSE;
    (void)comm_sync_notify(c);
    libos_syscall_relinquish_libos_lock(_offset);
}

static long forwarded_ioctl(unsigned int fd, unsigned int cmd, unsigned long arg) {
    const struct translated_ioctl* ti = find_translated_ioctl(cmd);
    int64_t saved[2] = {0};
    uint64_t time_field = 0;
    if (ti) {
        if (!is_user_memory_writable((void*)arg, _IOC_SIZE(cmd)))
            return -EFAULT;
        for (size_t i = 0; i < ti->fields_cnt; i++) {
            void* field = (void*)(arg + ti->fields[i].offset);
            switch (ti->fields[i].kind) {
                case FD_IN: {
                    saved[i] = *(int32_t*)field;
                    int in_fd = (int32_t)saved[i];
                    struct libos_handle* h = in_fd > (int)GRAPHICS_HOST_FD_OFFSET
                                             ? get_fd_handle(in_fd, NULL, NULL) : NULL;
                    if (h) {
                        if (h->type == TYPE_HOST)
                            *(int32_t*)field = in_fd - (int)GRAPHICS_HOST_FD_OFFSET;
                        put_handle(h);
                    }
                    break;
                }
                case TIME_ABS: {
                    saved[i] = *(int64_t*)field;
                    int64_t deadline = saved[i];
                    uint64_t now_us = 0;
                    if (deadline > 0 && deadline != INT64_MAX && PalSystemTimeQuery(&now_us) == 0) {
                        *(int64_t*)field = deadline - (int64_t)(now_us * 1000);
                        time_field = ti->fields[i].offset + 1;
                        log_debug("forwarded ioctl 0x%x: deadline %ld ns, %ld ns from now", cmd,
                                  (long)deadline, (long)*(int64_t*)field);
                    }
                    break;
                }
                case FD_OUT:
                    break;
            }
        }
    }

    long _offset = libos_syscall_acquire_libos_lock();
    comm_page_t* c = comm_page(_offset);
    c->p1 = (uint64_t)fd;
    c->p2 = (uint64_t)cmd;
    c->p3 = (uint64_t)arg;
    c->p4 = time_field;
    c->p5 = time_field ? GRAPHICS_IOCTL_TIME_REL_MAGIC : 0;
    c->req_bit = IOCTL;
    long ret = (long)comm_sync_notify(c);
    libos_syscall_relinquish_libos_lock(_offset);

    if (!ti)
        return ret;
    for (size_t i = 0; i < ti->fields_cnt; i++) {
        void* field = (void*)(arg + ti->fields[i].offset);
        if (ti->fields[i].kind == FD_IN) {
            *(int32_t*)field = (int32_t)saved[i];
            continue;
        }
        if (ti->fields[i].kind == TIME_ABS) {
            *(int64_t*)field = saved[i];
            continue;
        }
        if (ret < 0)
            continue;
        int host_fd = *(int32_t*)field;
        if (host_fd < 0)
            continue;
        bool cloexec = true;
        if (ti->cloexec_flags_offset >= 0)
            cloexec = *(uint32_t*)(arg + ti->cloexec_flags_offset) & DRM_CLOEXEC;
        int new_fd = install_host_fd(host_fd, cloexec);
        if (new_fd < 0) {
            log_warning("forwarded ioctl 0x%x: cannot install host fd %d: %d", cmd, host_fd,
                        new_fd);
            close_host_fd(host_fd);
            *(int32_t*)field = -1;
            ret = new_fd;
            continue;
        }
        *(int32_t*)field = new_fd;
    }
    return ret;
}
#endif

/* `cmd` arrives as the full register: glibc passes the request as an unsigned long (zero-extended),
 * while the RISC-V calling convention lets the compiler assume a 32-bit argument is sign-extended,
 * so a request with bit 31 set (`_IOR`, e.g. RNDGETENTCNT) would compare unequal to its own
 * constant. Narrowing it explicitly makes the value well-defined on every architecture. */
long libos_syscall_ioctl(unsigned int fd, unsigned long cmd_reg, unsigned long arg) {
    unsigned int cmd = (unsigned int)cmd_reg;
    struct libos_handle_map* handle_map = get_thread_handle_map(NULL);
    assert(handle_map);

    struct libos_handle* hdl = get_fd_handle(fd, NULL, handle_map);
    if (!hdl)
        return -EBADF;

    #ifdef ENABLE_GRAPHICS
        if(hdl->type == TYPE_HOST)
        {
            put_handle(hdl);
            return forwarded_ioctl(fd, cmd, arg);
        }
    #endif
    int ret;

    lock(&g_dcache_lock);
    bool is_host_dev = hdl->type == TYPE_CHROOT && hdl->dentry->inode &&
        hdl->dentry->inode->type == S_IFCHR;
    unlock(&g_dcache_lock);

    if (is_host_dev) {
        ret = raw_device_ioctl(hdl, cmd, arg);
        goto out;
    }

    switch (cmd) {
        case TIOCGPGRP:
            if (!hdl->uri || strcmp(hdl->uri, URI_PREFIX_CONSOLE)) {
                ret = -ENOTTY;
                break;
            }

            if (!is_user_memory_writable((void*)arg, sizeof(int))) {
                ret = -EFAULT;
                break;
            }
            rwlock_read_lock(&g_process_id_lock);
            *(int*)arg = g_process.pgid;
            rwlock_read_unlock(&g_process_id_lock);
            ret = 0;
            break;
        case FIONBIO:
            if (!is_user_memory_readable((void*)arg, sizeof(int))) {
                ret = -EFAULT;
                break;
            }
            int nonblocking_on = *(int*)arg;
            ret = set_handle_nonblocking(hdl, !!nonblocking_on);
            break;
        case FIONCLEX:
            rwlock_write_lock(&handle_map->lock);
            if (HANDLE_ALLOCATED(handle_map->map[fd])) {
                handle_map->map[fd]->flags &= ~FD_CLOEXEC;
                ret = 0;
            } else {
                ret = -EBADF;
            }
            rwlock_write_unlock(&handle_map->lock);
            break;
        case FIOCLEX:
            rwlock_write_lock(&handle_map->lock);
            if (HANDLE_ALLOCATED(handle_map->map[fd])) {
                handle_map->map[fd]->flags |= FD_CLOEXEC;
                ret = 0;
            } else {
                ret = -EBADF;
            }
            rwlock_write_unlock(&handle_map->lock);
            break;
        case FIOASYNC:
            ret = install_async_event(hdl->pal_handle, 0, &signal_io, NULL);
            break;
        case FIONREAD: {
            if (!is_user_memory_writable((void*)arg, sizeof(int))) {
                ret = -EFAULT;
                break;
            }

            struct libos_fs* fs = hdl->fs;
            if (!fs || !fs->fs_ops) {
                ret = -ENOTTY;
                break;
            }

            if (fs->fs_ops->ioctl) {
                ret = fs->fs_ops->ioctl(hdl, cmd, arg);
                break;
            }

            /* TODO: the code below should be deleted and each handle type should have custom ioctl
             * handling. */
            int size = 0;
            if (fs->fs_ops->hstat) {
                struct stat stat;
                ret = fs->fs_ops->hstat(hdl, &stat);
                if (ret < 0)
                    break;
                size = stat.st_size;
            } else if (hdl->pal_handle) {
                PAL_STREAM_ATTR attr;
                ret = PalStreamAttributesQueryByHandle(hdl->pal_handle, &attr);
                if (ret < 0) {
                    ret = pal_to_unix_errno(ret);
                    break;
                }
                size = attr.pending_size;
            }

            int offset = 0;
            if (fs->fs_ops->seek) {
                ret = fs->fs_ops->seek(hdl, 0, SEEK_CUR);
                if (ret < 0)
                    break;
                offset = ret;
            }

            *(int*)arg = size - offset;
            ret = 0;
            break;
        }
        default: {
            struct libos_fs* fs = hdl->fs;
            if (!fs || !fs->fs_ops || !fs->fs_ops->ioctl) {
                ret = -ENOTTY;
                break;
            }
            ret = fs->fs_ops->ioctl(hdl, cmd, arg);
            break;
        }
    }

out:
    put_handle(hdl);
    if (ret == -EINTR) {
        ret = -ERESTARTSYS;
    }
    return ret;
}
