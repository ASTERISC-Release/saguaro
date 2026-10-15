/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 Gramine contributors */

/* Waiting on streams in the vbs PAL proper: pipes (in-memory) and the console. A waiter sleeps on
 * the stream-change counter g_vbs_stream_gen, which every pipe change increments. */

#include "api.h"
#include "pal.h"
#include "pal_error.h"
#include "pal_internal.h"
#include "pal_vbs.h"

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
        case PAL_TYPE_CONSOLE:
            return PAL_WAIT_WRITE;
        case PAL_TYPE_FILE:
            return PAL_WAIT_READ;
        default:
            return 0;
    }
}

int _PalStreamsWaitEvents(size_t count, PAL_HANDLE* handle_array, pal_wait_flags_t* events,
                          pal_wait_flags_t* ret_events, uint64_t* timeout_us) {
    uint64_t start = vbs_time_ns();
    while (1) {
        uint32_t gen = __atomic_load_n(&g_vbs_stream_gen, __ATOMIC_ACQUIRE);
        bool any = false;
        for (size_t i = 0; i < count; i++) {
            ret_events[i] = 0;
            if (!handle_array[i])
                continue;
            pal_wait_flags_t r = ready(handle_array[i]);
            ret_events[i] = r & (events[i] | PAL_WAIT_ERROR | PAL_WAIT_HANG_UP);
            any |= ret_events[i] != 0;
        }
        if (any)
            return 0;
        uint64_t left_ns = UINT64_MAX;
        if (timeout_us) {
            uint64_t spent = vbs_time_ns() - start;
            uint64_t total = *timeout_us * 1000;
            if (spent >= total) {
                *timeout_us = 0;
                return -PAL_ERROR_TRYAGAIN;
            }
            left_ns = total - spent;
        }
        vbs_wait(&g_vbs_stream_gen, gen, left_ns);
    }
}
