/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 Gramine contributors */

/* Waiting on streams in the vbs PAL proper: pipes (in-memory) and the console. With one enclave
 * thread nothing changes while we wait, except through the timeout. */

#include "api.h"
#include "pal.h"
#include "pal_error.h"
#include "pal_internal.h"
#include "pal_vbs.h"

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
    uint64_t start = vbs_time_ns() / 1000;
    while (1) {
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
        if (!timeout_us) {
            log_error("vbs: waiting forever on streams with one enclave thread");
            return -PAL_ERROR_DENIED;
        }
        uint64_t now = vbs_time_ns() / 1000;
        if (now - start >= *timeout_us) {
            *timeout_us = 0;
            return -PAL_ERROR_TRYAGAIN;
        }
    }
}
