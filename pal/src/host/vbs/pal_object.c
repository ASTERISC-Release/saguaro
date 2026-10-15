/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/* Waiting on streams in the vbs PAL proper: pipes (in-memory), the console, files, and host
 * sockets. Pipes wake a waiter through the stream-change counter g_vbs_stream_gen; host sockets
 * are polled on the host (in slices of 10 ms when pipes are waited on too). */

#include "api.h"
#include "pal.h"
#include "pal_error.h"
#include "pal_internal.h"
#include "pal_linux_error.h"
#include "pal_vbs.h"

#define L_POLLIN   0x1
#define L_POLLOUT  0x4
#define L_POLLERR  0x8
#define L_POLLHUP  0x10
#define L_POLLNVAL 0x20
#define MAX_SOCKS  256
#define MIXED_SLICE_MS 10

uint32_t g_vbs_stream_gen;

void vbs_streams_changed(void) {
    __atomic_add_fetch(&g_vbs_stream_gen, 1, __ATOMIC_ACQ_REL);
    vbs_wake(&g_vbs_stream_gen, /*all=*/true);
}

static pal_wait_flags_t ready(PAL_HANDLE h) {
    switch (HANDLE_TYPE(h)) {
        case PAL_TYPE_PIPE:
        case PAL_TYPE_PIPESRV:
            return vbs_pipe_ready(h);
        case PAL_TYPE_EVENTFD:
            return vbs_eventfd_ready(h);
        case PAL_TYPE_CONSOLE:
            return PAL_WAIT_WRITE;
        case PAL_TYPE_FILE:
            return PAL_WAIT_READ | PAL_WAIT_WRITE;
        default:
            return 0;
    }
}

int _PalStreamsWaitEvents(size_t count, PAL_HANDLE* handle_array, pal_wait_flags_t* events,
                          pal_wait_flags_t* ret_events, uint64_t* timeout_us) {
    struct pal_vbs_host_pollfd* pf = NULL;
    size_t* pf_idx = NULL;
    size_t nsock = 0;
    bool local = false; /* in-enclave streams (pipes) are waited on too */
    for (size_t i = 0; i < count; i++) {
        if (!handle_array[i])
            continue;
        if (HANDLE_TYPE(handle_array[i]) == PAL_TYPE_SOCKET)
            nsock++;
        else
            local = true;
    }
    if (nsock > MAX_SOCKS)
        return -PAL_ERROR_INVAL;
    if (nsock) {
        pf = malloc(nsock * sizeof(*pf));
        pf_idx = malloc(nsock * sizeof(*pf_idx));
        if (!pf || !pf_idx) {
            free(pf);
            free(pf_idx);
            return -PAL_ERROR_NOMEM;
        }
    }

    int ret;
    uint64_t start = vbs_time_ns();
    while (1) {
        uint32_t gen = __atomic_load_n(&g_vbs_stream_gen, __ATOMIC_ACQUIRE);
        bool any = false;
        size_t k = 0;
        for (size_t i = 0; i < count; i++) {
            ret_events[i] = 0;
            if (!handle_array[i])
                continue;
            if (HANDLE_TYPE(handle_array[i]) == PAL_TYPE_SOCKET) {
                pf[k].id = handle_array[i]->sock.host_id;
                pf[k].events = ((events[i] & PAL_WAIT_READ) ? L_POLLIN : 0) |
                               ((events[i] & PAL_WAIT_WRITE) ? L_POLLOUT : 0);
                pf[k].revents = 0;
                pf_idx[k++] = i;
                continue;
            }
            pal_wait_flags_t r = ready(handle_array[i]);
            ret_events[i] = r & (events[i] | PAL_WAIT_ERROR | PAL_WAIT_HANG_UP);
            any |= ret_events[i] != 0;
        }

        uint64_t left_ns = UINT64_MAX;
        if (timeout_us) {
            uint64_t spent = vbs_time_ns() - start;
            uint64_t total = *timeout_us * 1000;
            left_ns = spent >= total ? 0 : total - spent;
        }

        if (nsock) {
            int64_t ms = any ? 0 : (left_ns == UINT64_MAX ? 1000 : (int64_t)(left_ns / 1000000));
            if (local && ms > MIXED_SLICE_MS)
                ms = MIXED_SLICE_MS;
            size_t n = 0;
            int64_t r = vbs_hostcall(PAL_VBS_HOST_POLL, nsock, ms, 0, 0, pf, nsock * sizeof(*pf),
                                     pf, nsock * sizeof(*pf), &n);
            if (r < 0) {
                ret = unix_to_pal_error((int)r);
                goto out;
            }
            for (size_t j = 0; j < nsock && (j + 1) * sizeof(*pf) <= n; j++) {
                uint32_t re = pf[j].revents;
                pal_wait_flags_t f = 0;
                if (re & L_POLLIN)
                    f |= PAL_WAIT_READ;
                if (re & L_POLLOUT)
                    f |= PAL_WAIT_WRITE;
                if (re & (L_POLLERR | L_POLLNVAL))
                    f |= PAL_WAIT_ERROR;
                if (re & L_POLLHUP)
                    f |= PAL_WAIT_HANG_UP;
                size_t i = pf_idx[j];
                ret_events[i] = f & (events[i] | PAL_WAIT_ERROR | PAL_WAIT_HANG_UP);
                any |= ret_events[i] != 0;
            }
        }
        if (any) {
            ret = 0;
            goto out;
        }
        if (timeout_us) {
            uint64_t spent = vbs_time_ns() - start;
            if (spent >= *timeout_us * 1000) {
                *timeout_us = 0;
                ret = -PAL_ERROR_TRYAGAIN;
                goto out;
            }
        }
        if (!nsock && left_ns >= VBS_SHORT_WAIT_NS)
            vbs_wait(&g_vbs_stream_gen, gen, left_ns); /* shorter waits poll (pal_events.c) */
    }
out:
    free(pf);
    free(pf_idx);
    return ret;
}
