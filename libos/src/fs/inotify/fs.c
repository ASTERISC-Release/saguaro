/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/*
 * Filesystem of inotify instances, the same "emulate in LibOS" mode as eventfd/timerfd: the event
 * queue is entirely inside the LibOS (`sys/libos_inotify.c` fills it), and a dummy host eventfd
 * exists only to wake up blocking read/poll/select/epoll. `post_poll` decides readability from
 * the queue, as for the others.
 */

#include "libos_fs.h"
#include "libos_handle.h"
#include "libos_inotify.h"
#include "libos_internal.h"
#include "libos_utils.h"
#include "pal.h"
#include "perm.h"
#include "stat.h"

static int inotify_checkin(struct libos_handle* hdl) {
    assert(hdl->type == TYPE_INOTIFY);
    /* like eventfd/timerfd: watches and queues live in the creating process only */
    hdl->info.inotify.broken_in_child = true;
    return 0;
}

static ssize_t inotify_read(struct libos_handle* hdl, void* buf, size_t count,
                            file_off_t* pos) {
    __UNUSED(pos);
    assert(hdl->type == TYPE_INOTIFY);
    struct libos_inotify_handle* n = &hdl->info.inotify;

    if (hdl->info.inotify.broken_in_child) {
        log_warning("Child process tried to read an inotify instance created by its parent. This "
                    "is currently unsupported");
        return -EACCES;
    }

    /* the kernel checks the buffer size before anything else */
    if (count < INOTIFY_EVENT_HDRLEN)
        return -EINVAL;

    size_t copied = 0;
    while (true) {
        spinlock_lock(&n->lock);
        struct libos_inotify_event* ev = n->ev_head;
        if (!ev) {
            bool nonblock = (hdl->flags & O_NONBLOCK) != 0;
            spinlock_unlock(&n->lock);
            if (nonblock || copied)
                break; /* EAGAIN, or return what we already delivered */
            /* block until an event arrives; spurious wakeups just recheck the queue */
            pal_wait_flags_t wait_for = PAL_WAIT_READ;
            pal_wait_flags_t ret_events = 0;
            int ret = PalStreamsWaitEvents(1, &hdl->pal_handle, &wait_for, &ret_events,
                                           /*timeout=*/NULL);
            if (ret < 0 && ret != -PAL_ERROR_TRYAGAIN)
                return copied ? (ssize_t)copied : pal_to_unix_errno(ret);
            continue;
        }

        size_t ev_size = INOTIFY_EVENT_HDRLEN + ev->len;
        if (ev_size > count - copied) {
            /* the next event does not fit: never deliver a partial event (kernel semantics) */
            spinlock_unlock(&n->lock);
            break;
        }

        n->ev_head = ev->next;
        if (!n->ev_head)
            n->ev_tail = NULL;
        n->queued--;
        bool drained = n->queued == 0;
        if (drained && n->dummy_host_val != 0)
            n->dummy_host_val = 0;

        /* serialize like the kernel: wd, mask, cookie, len, then the padded name */
        struct {
            int wd;
            unsigned int mask;
            unsigned int cookie;
            unsigned int len;
        } hdr = { .wd = ev->wd, .mask = ev->mask, .cookie = ev->cookie, .len = ev->len };
        spinlock_unlock(&n->lock);

        memcpy((char*)buf + copied, &hdr, sizeof(hdr));
        if (ev->len)
            memcpy((char*)buf + copied + INOTIFY_EVENT_HDRLEN, ev->name, ev->len);
        copied += ev_size;
        free(ev);

        if (drained) {
            /* consume our host notification so level-triggered poll matches the queue */
            uint64_t host_val = 0;
            size_t read_size = sizeof(host_val);
            int ret = PalStreamRead(hdl->pal_handle, /*offset=*/0, &read_size, &host_val);
            if (ret < 0) {
                log_debug("inotify: failed to drain the host notification: %s",
                          pal_strerror(ret));
            }
            break;
        }
    }

    if (!copied && (hdl->flags & O_NONBLOCK))
        return -EAGAIN;

    if (copied)
        maybe_epoll_et_trigger(hdl, copied, /*in=*/true, /*was_partial=*/false);
    return copied;
}

static ssize_t inotify_write(struct libos_handle* hdl, const void* buf, size_t count,
                             file_off_t* pos) {
    __UNUSED(hdl);
    __UNUSED(buf);
    __UNUSED(count);
    __UNUSED(pos);
    /* the kernel refuses writes to an inotify fd with EINVAL */
    return -EINVAL;
}

static void inotify_post_poll(struct libos_handle* hdl, pal_wait_flags_t* pal_ret_events) {
    struct libos_inotify_handle* n = &hdl->info.inotify;

    if (n->broken_in_child) {
        *pal_ret_events = 0;
        return;
    }

    if (*pal_ret_events & PAL_WAIT_READ) {
        /* only report readable when the queue really holds an event */
        spinlock_lock(&n->lock);
        bool readable = n->queued != 0;
        spinlock_unlock(&n->lock);
        if (!readable)
            *pal_ret_events &= ~PAL_WAIT_READ;
    }
    /* unlike timerfd, the kernel's inotify poll does report writability */
}

static int inotify_hstat(struct libos_handle* hdl, struct stat* stat) {
    __UNUSED(hdl);
    memset(stat, 0, sizeof(*stat));
    /* the kernel reports the inotify fd as its misc character device (major 10, minor 212); the
     * encoding matches the `makedev()` of `fs/libos_fs_pseudo.c` */
    stat->st_mode = S_IFCHR | PERM_rw_______;
    stat->st_rdev = (uint64_t)10 << 8 | 212;
    stat->st_nlink = 1;
    return 0;
}

static int inotify_close(struct libos_handle* hdl) {
    libos_inotify_destroy_watches(hdl);
    return 0;
}

struct libos_fs_ops inotify_fs_ops = {
    .checkin   = &inotify_checkin,
    .close     = &inotify_close,
    .read      = &inotify_read,
    .write     = &inotify_write,
    .post_poll = &inotify_post_poll,
    .hstat     = &inotify_hstat,
};

struct libos_fs inotify_builtin_fs = {
    .name   = "inotify",
    .fs_ops = &inotify_fs_ops,
};
