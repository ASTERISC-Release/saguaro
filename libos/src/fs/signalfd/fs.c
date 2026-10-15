/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/*
 * Filesystem of signalfd handles (see `sys/libos_signalfd.c` for the design). The read takes
 * siginfos of pending signals that the reading thread blocks and this descriptor watches, out of
 * the pending queues themselves, and formats them as `struct signalfd_siginfo`.
 */

#include "libos_checkpoint.h"
#include "libos_fs.h"
#include "libos_handle.h"
#include "libos_internal.h"
#include "libos_thread.h"
#include "libos_utils.h"
#include "linux_abi/signals.h"
#include "linux_abi/signalfd.h"
#include "pal.h"

/* fill one `struct signalfd_siginfo` from a `siginfo_t` (unset fields stay zero) */
static void signalfd_fill_siginfo(const siginfo_t* si, struct signalfd_siginfo* ssi) {
    memset(ssi, 0, sizeof(*ssi));
    ssi->ssi_signo = si->si_signo;
    ssi->ssi_errno = si->si_errno;
    ssi->ssi_code  = si->si_code;

    /* sender info is present for the queued/kill codes */
    switch (si->si_code) {
        case SI_USER:
        case SI_TKILL:
        case SI_QUEUE:
        case SI_ASYNCIO:
        case SI_MESGQ:
        case SI_TIMER:
            ssi->ssi_pid = si->si_pid;
            ssi->ssi_uid = si->si_uid;
            break;
    }
    switch (si->si_code) {
        case SI_QUEUE:
            ssi->ssi_int = (int32_t)si->si_value.sival_int;
            ssi->ssi_ptr = (uint64_t)(uintptr_t)si->si_value.sival_ptr;
            break;
        case SI_TIMER:
            ssi->ssi_tid = (uint32_t)si->si_tid;
            ssi->ssi_overrun = (uint32_t)si->si_overrun;
            break;
        default:
            break;
    }
    if (si->si_signo == SIGCHLD)
        ssi->ssi_status = si->si_status;
    /* positive `si_code` values are the fault/trap codes, which carry the fault address */
    if (si->si_code > 0)
        ssi->ssi_addr = (uint64_t)(uintptr_t)si->si_addr;
}

static ssize_t signalfd_read(struct libos_handle* hdl, void* buf, size_t count,
                             file_off_t* pos) {
    struct libos_signalfd_handle* s = &hdl->info.signalfd;

    if (s->broken_in_child) {
        log_warning("Child process tried to access a signalfd created by its parent. This is "
                    "currently unsupported (may lead to segfault)");
        return -EACCES;
    }

    if (count < sizeof(struct signalfd_siginfo)
            || count % sizeof(struct signalfd_siginfo) != 0)
        return -EINVAL;

    struct signalfd_siginfo* out = buf;
    size_t max = count / sizeof(struct signalfd_siginfo);
    size_t filled = 0;

    while (filled < max) {
        /* only signals the reading thread currently blocks are taken, as on Linux */
        __sigset_t blocked;
        get_sig_mask(get_cur_thread(), &blocked);

        __sigset_t match;
        __sigemptyset(&match);
        for (int sig = 1; sig <= SIGS_CNT; sig++) {
            if (__sigismember(&s->mask, sig) && __sigismember(&blocked, sig))
                __sigaddset(&match, sig);
        }

        siginfo_t si;
        if (!__sigisemptyset(&match) && signalfd_take_signal(&match, &si)) {
            signalfd_fill_siginfo(&si, &out[filled++]);
            continue;
        }

        if (filled)
            break;

        if (hdl->flags & O_NONBLOCK)
            return -EAGAIN;

        /* block until some signal arrives (the wake is a hint from
         * `libos_signalfd_notify()`; a signal that became pending without a hint — e.g. delivered
         * to another thread first — simply means one more wait, as the pending queue is the
         * source of truth) */
        pal_wait_flags_t wait_for = PAL_WAIT_READ;
        pal_wait_flags_t ret_events = 0;
        int ret = PalStreamsWaitEvents(1, &hdl->pal_handle, &wait_for, &ret_events,
                                       /*timeout=*/NULL);
        if (ret < 0 && ret != -PAL_ERROR_TRYAGAIN)
            return pal_to_unix_errno(ret);

        /* consume the hint */
        spinlock_lock(&s->lock);
        bool drain_host = s->dummy_host_val != 0;
        if (drain_host)
            s->dummy_host_val = 0;
        spinlock_unlock(&s->lock);
        if (drain_host) {
            uint64_t host_val = 0;
            size_t read_size = sizeof(host_val);
            PalStreamRead(hdl->pal_handle, /*offset=*/0, &read_size, &host_val);
        }
    }

    if (filled)
        maybe_epoll_et_trigger(hdl, (int)(filled * sizeof(*out)), /*in=*/true,
                               /*unused was_partial=*/false);
    return (ssize_t)(filled * sizeof(*out));
}

static ssize_t signalfd_write(struct libos_handle* hdl, const void* buf, size_t count,
                              file_off_t* pos) {
    __UNUSED(hdl);
    __UNUSED(buf);
    __UNUSED(count);
    __UNUSED(pos);
    return -EBADF; /* a signalfd is read-only */
}

static void signalfd_post_poll(struct libos_handle* hdl, pal_wait_flags_t* pal_ret_events) {
    struct libos_signalfd_handle* s = &hdl->info.signalfd;

    if (s->broken_in_child) {
        *pal_ret_events = 0;
        return;
    }

    /* readable iff an arrival hint is outstanding; the actual matching is done by the read (the
     * hint can be stale, the read then blocks again, this is a race the kernel has too) */
    if (*pal_ret_events & PAL_WAIT_READ) {
        spinlock_lock(&s->lock);
        bool readable = s->dummy_host_val != 0;
        spinlock_unlock(&s->lock);
        if (!readable)
            *pal_ret_events &= ~PAL_WAIT_READ;
    }
    *pal_ret_events &= ~PAL_WAIT_WRITE;
}

static int signalfd_checkin(struct libos_handle* hdl) {
    assert(hdl->type == TYPE_SIGNALFD);
    hdl->info.signalfd.broken_in_child = true;
    return 0;
}

static int signalfd_close(struct libos_handle* hdl) {
    libos_signalfd_untrack(hdl);
    return 0;
}

struct libos_fs_ops signalfd_fs_ops = {
    .checkin   = &signalfd_checkin,
    .close     = &signalfd_close,
    .read      = &signalfd_read,
    .write     = &signalfd_write,
    .post_poll = &signalfd_post_poll,
};

struct libos_fs signalfd_builtin_fs = {
    .name   = "signalfd",
    .fs_ops = &signalfd_fs_ops,
};
