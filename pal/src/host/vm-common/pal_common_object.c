/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2023 Intel Corporation */

/*
 * This file contains APIs for polling PAL handles.
 */

#include "api.h"
#include "list.h"
#include "pal.h"
#include "pal_common.h"
#include "pal_error.h"
#include "pal_internal.h"
#if defined(__x86_64__) || defined(__aarch64__)
#include "kernel_memory.h"
#endif
#include "spinlock.h"

#include "kernel_sched.h"
#include "kernel_time.h"
#include "kernel_virtio.h"

/* Global lock and futex for waiting for some events (very naive but good enough) */
static spinlock_t g_streams_waiting_events_lock = INIT_SPINLOCK_UNLOCKED;
int g_streams_waiting_events_futex;

static int check_pipesrv_handle(struct pal_handle* handle, pal_wait_flags_t events,
                                pal_wait_flags_t* out_events) {
    assert(handle->hdr.type == PAL_TYPE_PIPESRV);

    pal_wait_flags_t revents = 0;

    spinlock_lock(&g_connecting_pipes_lock);
    if ((events & PAL_WAIT_READ)) {
        bool any_connecting_pipe_found = false;

        struct pal_handle* connecting_pipe = NULL;
        LISTP_FOR_EACH_ENTRY(connecting_pipe, &g_connecting_pipes_list, list) {
            if (strcmp(connecting_pipe->pipe.name, handle->pipe.name) == 0) {
                any_connecting_pipe_found = true;
                break;
            }
        }

        if (any_connecting_pipe_found)
            revents |= PAL_WAIT_READ;
    }
    handle->pipe.connect_poll_waiting = (revents == 0);
    spinlock_unlock(&g_connecting_pipes_lock);

    *out_events = revents;
    return 0;
}

static int check_pipe_handle(struct pal_handle* handle, pal_wait_flags_t events,
                             pal_wait_flags_t* out_events) {
    assert(handle->hdr.type == PAL_TYPE_PIPECLI || handle->hdr.type == PAL_TYPE_PIPE);

    struct pal_handle_inner_pipe_buf* pipe_buf = handle->pipe.pipe_buf;
    if (!pipe_buf) {
        *out_events = 0;
        return 0;
    }

    pal_wait_flags_t revents = 0;

    spinlock_lock(&pipe_buf->lock);
    /* this end reads one ring and writes the other (see pal_common_pipes.c) */
    const struct pal_pipe_ring* rr = &pipe_buf->ring[handle->hdr.type == PAL_TYPE_PIPE ? 1 : 0];
    const struct pal_pipe_ring* wr = &pipe_buf->ring[handle->hdr.type == PAL_TYPE_PIPE ? 0 : 1];
    bool pending = rr->read_pos != rr->write_pos;

    if (!rr->open_write && !wr->open_read) {
        /* the other end is gone (or both sides were shut down): as Linux reports POLLIN before
         * POLLHUP, what is left to read comes first (with processes, the last message of an
         * exiting peer, sent just before its pipes close, must not be hidden) */
        if ((events & PAL_WAIT_READ) && pending && rr->open_read) {
            revents = PAL_WAIT_READ;
            goto out;
        }
        handle->flags |= PAL_HANDLE_FD_ERROR; /* TODO: maybe PAL_HANDLE_FD_HANG_UP? */
        revents = PAL_WAIT_ERROR;             /* TODO: maybe PAL_WAIT_HANG_UP? */
        goto out;
    }

    if ((events & PAL_WAIT_READ) && rr->open_read) {
        /* read event requested, and this end is open for read: something to read, or the other
         * end closed its writing side (reads return end-of-stream at once) */
        if (pending || !rr->open_write) {
            revents |= PAL_WAIT_READ;
        }
    }

    if ((events & PAL_WAIT_WRITE) && wr->open_write) {
        /* write event requested, and this end is open for write: room to write, or the other
         * end closed its reading side (writes fail at once) */
        if (wr->write_pos - wr->read_pos < PIPE_BUF_SIZE || !wr->open_read) {
            revents |= PAL_WAIT_WRITE;
        }
    }

out:
    pipe_buf->poll_waiting = (revents == 0);
    spinlock_unlock(&pipe_buf->lock);

    *out_events = revents;
    return 0;
}

static int check_socket_handle(struct pal_handle* handle, pal_wait_flags_t events,
                               pal_wait_flags_t* out_events) {
    return pal_common_socket_wait_events(handle, events, out_events);
}

static int check_eventfd_handle(struct pal_handle* handle, pal_wait_flags_t events,
                                pal_wait_flags_t* out_events) {
    assert(handle->hdr.type == PAL_TYPE_EVENTFD);

    spinlock_lock(&handle->eventfd.lock);

    pal_wait_flags_t revents = 0;
    if ((events & PAL_WAIT_READ) && handle->eventfd.val) {
        revents |= PAL_WAIT_READ;
    }
    if (events & PAL_WAIT_WRITE && handle->eventfd.val < UINT64_MAX - 1) {
        revents |= PAL_WAIT_WRITE;
    }

    handle->eventfd.poll_waiting = (revents == 0);
    *out_events = revents;
    spinlock_unlock(&handle->eventfd.lock);
    return 0;
}

static int check_handle(struct pal_handle* handle, pal_wait_flags_t events,
                        pal_wait_flags_t* out_events) {
    if (!handle) {
        *out_events = PAL_WAIT_ERROR;
        return 0;
    }

    if (handle->hdr.type == PAL_TYPE_PIPESRV) {
        return check_pipesrv_handle(handle, events, out_events);
    } else if (handle->hdr.type == PAL_TYPE_PIPECLI || handle->hdr.type == PAL_TYPE_PIPE) {
        return check_pipe_handle(handle, events, out_events);
    } else if (handle->hdr.type == PAL_TYPE_SOCKET) {
        return check_socket_handle(handle, events, out_events);
    } else if (handle->hdr.type == PAL_TYPE_EVENTFD) {
        return check_eventfd_handle(handle, events, out_events);
    }

    /* cannot recognize this handle */
    return -PAL_ERROR_INVAL;
}

int pal_common_streams_wait_events(size_t count, struct pal_handle** handle_array,
                                   pal_wait_flags_t* events, pal_wait_flags_t* ret_events,
                                   uint64_t* timeout_us) {
    int ret;
    bool any_event_found = false;
    uint64_t timeout_absolute_us = 0;
    void* timeout = NULL;

    spinlock_lock(&g_streams_waiting_events_lock);

    if (timeout_us && *timeout_us != 0) {
        uint64_t curr_time_us;
        ret = get_time_in_us(&curr_time_us);
        if (ret < 0) {
            spinlock_unlock(&g_streams_waiting_events_lock);
            return ret;
        }

        timeout_absolute_us = curr_time_us + *timeout_us;
        ret = register_timeout(timeout_absolute_us, &g_streams_waiting_events_futex, &timeout);
        if (ret < 0) {
            spinlock_unlock(&g_streams_waiting_events_lock);
            return ret;
        }
    }

    while (!any_event_found) {
        /* Active event loops poll shared VSOCK rings with notifications masked, avoiding a VM exit
         * per X11 packet. Notifications are rearmed below only if this wait must sleep. */
        (void)virtio_vsock_poll();

        for (size_t i = 0; i < count; i++) {
            ret_events[i] = 0;

            pal_wait_flags_t revents = 0;
            ret = check_handle(handle_array[i], events[i], &revents);
            if (ret < 0)
                goto out;

            if (revents) {
                ret_events[i] = revents;
                any_event_found = true;
            }
        }

        if (any_event_found)
            break;

        if (timeout_us) {
            if (*timeout_us == 0) {
                ret = -PAL_ERROR_TRYAGAIN;
                goto out;
            }

            /* check if timeout expired */
            assert(timeout_absolute_us);

            uint64_t curr_time_us;
            ret = get_time_in_us(&curr_time_us);
            if (ret < 0)
                goto out;

            if (timeout_absolute_us <= curr_time_us) {
                ret = -PAL_ERROR_TRYAGAIN;
                goto out;
            }
        }

        /* no events found, need to sleep for any new events */
        if (virtio_vsock_rearm_notifications())
            continue;
        sched_thread_wait(&g_streams_waiting_events_futex, &g_streams_waiting_events_lock);
    }

    ret = 0;
out:
    spinlock_unlock(&g_streams_waiting_events_lock);

    if (timeout)
        deregister_timeout(timeout);

    if (timeout_us && *timeout_us != 0) {
        uint64_t curr_us;
        int get_time_ret = get_time_in_us(&curr_us);
        if (!get_time_ret) {
            *timeout_us = timeout_absolute_us > curr_us ? timeout_absolute_us - curr_us : 0;
        }
    }

    return ret;
}
