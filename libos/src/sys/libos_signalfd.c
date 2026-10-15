/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/*
 * Implementation of the system calls "signalfd" and "signalfd4".
 *
 * Like eventfd/timerfd, a signalfd is a LibOS object with a dummy host eventfd used purely to wake
 * up blocking reads and poll/epoll. The siginfos are *not* copied at signal-arrival time: a
 * signalfd read takes them from the very thread/process pending queues the kernel would take them
 * from (blocked by the reading thread and watched by this descriptor), exactly as Linux's signalfd
 * does. Arrival of any pending signal only writes the wake notifications (`libos_signalfd_notify`
 * from `bookkeep/libos_signal.c`).
 */

#include "libos_checkpoint.h"
#include "libos_fs.h"
#include "libos_handle.h"
#include "libos_internal.h"
#include "libos_table.h"
#include "libos_utils.h"
#include "linux_abi/signals.h"
#include "linux_abi/signalfd.h"
#include "pal.h"

#define SIGNALFD_MAX 32

static struct libos_handle* g_signalfds[SIGNALFD_MAX];
static size_t g_signalfds_count;
static spinlock_t g_signalfds_lock = INIT_SPINLOCK_UNLOCKED;

static int signalfd_add_track(struct libos_handle* hdl) {
    int ret = 0;
    spinlock_lock(&g_signalfds_lock);
    if (g_signalfds_count == SIGNALFD_MAX) {
        ret = -ENFILE;
    } else {
        g_signalfds[g_signalfds_count++] = hdl;
    }
    spinlock_unlock(&g_signalfds_lock);
    return ret;
}

static void signalfd_remove_track(struct libos_handle* hdl) {
    spinlock_lock(&g_signalfds_lock);
    for (size_t i = 0; i < g_signalfds_count; i++) {
        if (g_signalfds[i] == hdl) {
            g_signalfds[i] = g_signalfds[--g_signalfds_count];
            break;
        }
    }
    spinlock_unlock(&g_signalfds_lock);
}

void libos_signalfd_untrack(struct libos_handle* hdl) {
    signalfd_remove_track(hdl);
}

/* Called (with no locks held) after a signal became pending: wake the dummy host eventfds of the
 * signalfds that watch this signal, so that blocking reads and polls recheck the queues. */
void libos_signalfd_notify(int sig) {
    if (sig < 1 || sig > SIGS_CNT)
        return;

    spinlock_lock(&g_signalfds_lock);
    for (size_t i = 0; i < g_signalfds_count; i++) {
        struct libos_handle* hdl = g_signalfds[i];
        struct libos_signalfd_handle* s = &hdl->info.signalfd;

        if (s->broken_in_child || !__sigismember(&s->mask, sig))
            continue;

        bool notify_host = false;
        spinlock_lock(&s->lock);
        if (s->dummy_host_val == 0) {
            s->dummy_host_val = 1;
            notify_host = true;
        }
        spinlock_unlock(&s->lock);

        if (notify_host && hdl->pal_handle) {
            uint64_t one = 1;
            size_t size = sizeof(one);
            PalStreamWrite(hdl->pal_handle, /*offset=*/0, &size, &one);
        }
    }
    spinlock_unlock(&g_signalfds_lock);
}

/* Shared body of `signalfd()`/`signalfd4()`; replaces the mask of an existing descriptor if `fd`
 * is one. */
static long do_signalfd(int fd, const __sigset_t* mask, size_t sizemask, int flags) {
    if (sizemask != sizeof(__sigset_t))
        return -EINVAL;

    if (flags & ~(SFD_CLOEXEC | SFD_NONBLOCK))
        return -EINVAL;

    __sigset_t new_mask;
    if (!is_user_memory_readable((void*)mask, sizemask))
        return -EFAULT;
    new_mask = *mask;
    /* the kernel silently drops the two unblockable signals */
    __sigdelset(&new_mask, SIGKILL);
    __sigdelset(&new_mask, SIGSTOP);

    if (fd >= 0) {
        struct libos_handle* hdl = get_fd_handle(fd, NULL, NULL);
        if (!hdl)
            return -EBADF;
        int ret;
        if (hdl->type != TYPE_SIGNALFD) {
            ret = -EINVAL;
        } else if (hdl->info.signalfd.broken_in_child) {
            ret = -EINVAL;
        } else {
            hdl->info.signalfd.mask = new_mask;
            hdl->flags = (hdl->flags & ~O_NONBLOCK) | (flags & SFD_NONBLOCK ? O_NONBLOCK : 0);
            ret = fd;
        }
        put_handle(hdl);
        return ret;
    }

    struct libos_handle* hdl = get_new_handle();
    if (!hdl)
        return -ENOMEM;

    hdl->type = TYPE_SIGNALFD;
    hdl->fs = &signalfd_builtin_fs;
    hdl->flags = O_RDONLY | (flags & SFD_NONBLOCK ? O_NONBLOCK : 0);
    hdl->acc_mode = MAY_READ;

    hdl->info.signalfd.broken_in_child = false;
    hdl->info.signalfd.mask = new_mask;
    hdl->info.signalfd.dummy_host_val = 0;

    int ret = PalStreamOpen(URI_PREFIX_EVENTFD, PAL_ACCESS_RDWR, /*share_flags=*/0,
                            PAL_CREATE_IGNORED, /*flags=*/0, &hdl->pal_handle);
    if (ret < 0) {
        ret = pal_to_unix_errno(ret);
        goto out;
    }

    ret = signalfd_add_track(hdl);
    if (ret < 0)
        goto out;

    ret = set_new_fd_handle(hdl, flags & SFD_CLOEXEC ? FD_CLOEXEC : 0, NULL);
out:
    put_handle(hdl);
    return ret;
}

long libos_syscall_signalfd(int fd, const __sigset_t* mask, size_t sizemask) {
    /* the pre-`signalfd4` entry point: same semantics, no flags */
    return do_signalfd(fd, mask, sizemask, 0);
}

long libos_syscall_signalfd4(int fd, const __sigset_t* mask, size_t sizemask, int flags) {
    return do_signalfd(fd, mask, sizemask, flags);
}
