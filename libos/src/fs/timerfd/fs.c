/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/*
 * Filesystem of timerfd handles, mirroring the "emulate in LibOS" mode of eventfd: the expiration
 * bookkeeping is entirely inside the LibOS (the timer worker thread in `sys/libos_timerfd.c`
 * computes the expirations), and a dummy host eventfd object exists only to wake up blocking
 * read/poll/select/epoll. `post_poll` drops spurious read notifications, as for eventfd.
 */

#include "libos_checkpoint.h"
#include "libos_fs.h"
#include "libos_handle.h"
#include "libos_internal.h"
#include "libos_utils.h"
#include "linux_abi/time.h"
#include "pal.h"

static int timerfd_checkin(struct libos_handle* hdl) {
    assert(hdl->type == TYPE_TIMERFD);

    /* Like eventfd: timerfds created by a process do not work in its children (the expiration
     * worker thread only exists in the creating process). */
    hdl->info.timerfd.broken_in_child = true;
    return 0;
}

static ssize_t timerfd_read(struct libos_handle* hdl, void* buf, size_t count,
                            file_off_t* pos) {
    __UNUSED(pos);
    struct libos_timerfd_handle* t = &hdl->info.timerfd;

    if (count < sizeof(uint64_t))
        return -EINVAL;

    if (t->broken_in_child) {
        log_warning("Child process tried to access a timerfd created by its parent. This is "
                    "currently unsupported (may lead to segfault)");
        return -EACCES;
    }

    uint64_t expirations;
    while (true) {
        spinlock_lock(&t->lock);
        expirations = t->expirations;
        spinlock_unlock(&t->lock);

        if (expirations)
            break;

        if (hdl->flags & O_NONBLOCK)
            return -EAGAIN;

        /* block until the worker thread fires this timer (a dummy host eventfd write); a spurious
         * or early notification just makes us recheck the expiration bookkeeping below */
        pal_wait_flags_t wait_for = PAL_WAIT_READ;
        pal_wait_flags_t ret_events = 0;
        int ret = PalStreamsWaitEvents(1, &hdl->pal_handle, &wait_for, &ret_events,
                                       /*timeout=*/NULL);
        if (ret < 0 && ret != -PAL_ERROR_TRYAGAIN)
            return pal_to_unix_errno(ret);
    }

    /* consume the expirations and the matching host notifications (the worker wrote one host
     * event when it accumulated the expirations; drain it so that level polling reflects the
     * current state) */
    spinlock_lock(&t->lock);
    expirations = t->expirations;
    t->expirations = 0;
    bool drain_host = t->dummy_host_val != 0;
    if (drain_host)
        t->dummy_host_val = 0;
    spinlock_unlock(&t->lock);

    if (drain_host) {
        uint64_t host_val = 0;
        size_t read_size = sizeof(host_val);
        int ret = PalStreamRead(hdl->pal_handle, /*offset=*/0, &read_size, &host_val);
        if (ret < 0) {
            /* the host eventfd must have been readable (we wrote it); a zero read is the host
             * malfunctioning, the notification is then simply left for the next poll */
            log_debug("timerfd: failed to drain the host notification: %s", pal_strerror(ret));
        }
    }

    /* the kernel reports at most UINT64_MAX expirations */
    memcpy(buf, &expirations, sizeof(expirations));

    maybe_epoll_et_trigger(hdl, sizeof(expirations), /*in=*/true, /*unused was_partial=*/false);
    return sizeof(expirations);
}

static ssize_t timerfd_write(struct libos_handle* hdl, const void* buf, size_t count,
                             file_off_t* pos) {
    __UNUSED(hdl);
    __UNUSED(buf);
    __UNUSED(count);
    __UNUSED(pos);
    /* the kernel refuses writes to a timerfd with EINVAL */
    return -EINVAL;
}

static void timerfd_post_poll(struct libos_handle* hdl, pal_wait_flags_t* pal_ret_events) {
    assert(*pal_ret_events && !(*pal_ret_events & ~PAL_WAIT_ALL));

    struct libos_timerfd_handle* t = &hdl->info.timerfd;

    if (t->broken_in_child) {
        *pal_ret_events = 0;
        return;
    }

    if (*pal_ret_events & PAL_WAIT_READ) {
        /* only report readable when an expiration is really unread, the host may notify early */
        spinlock_lock(&t->lock);
        bool readable = t->expirations != 0;
        spinlock_unlock(&t->lock);
        if (!readable)
            *pal_ret_events &= ~PAL_WAIT_READ;
    }

    /* the kernel's timerfd poll never reports writability */
    *pal_ret_events &= ~PAL_WAIT_WRITE;
}

static int timerfd_close(struct libos_handle* hdl) {
    /* drop the worker registry reference if the timer is still armed (idempotent) */
    return libos_timerfd_untrack(hdl);
}

struct libos_fs_ops timerfd_fs_ops = {
    .checkin   = &timerfd_checkin,
    .close     = &timerfd_close,
    .read      = &timerfd_read,
    .write     = &timerfd_write,
    .post_poll = &timerfd_post_poll,
};

struct libos_fs timerfd_builtin_fs = {
    .name   = "timerfd",
    .fs_ops = &timerfd_fs_ops,
};
