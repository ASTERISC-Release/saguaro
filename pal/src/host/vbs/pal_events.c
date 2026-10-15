/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 Gramine contributors */

/* Events of the vbs PAL proper. With one enclave thread a wait on an unsignaled event can only end
 * by its timeout; the time comes from the host clock. (Blocking with WaitOnAddress across enclave
 * threads comes with threads, phase 3.) */

#include "api.h"
#include "pal.h"
#include "pal_error.h"
#include "pal_internal.h"
#include "pal_vbs.h"

int _PalEventCreate(PAL_HANDLE* handle_ptr, bool init_signaled, bool auto_clear) {
    PAL_HANDLE handle = calloc(1, HANDLE_SIZE(event));
    if (!handle)
        return -PAL_ERROR_NOMEM;
    init_handle_hdr(handle, PAL_TYPE_EVENT);
    spinlock_init(&handle->event.lock);
    handle->event.signaled = init_signaled;
    handle->event.auto_clear = auto_clear;
    *handle_ptr = handle;
    return 0;
}

void _PalEventSet(PAL_HANDLE handle) {
    spinlock_lock(&handle->event.lock);
    handle->event.signaled = 1;
    spinlock_unlock(&handle->event.lock);
}

void _PalEventClear(PAL_HANDLE handle) {
    spinlock_lock(&handle->event.lock);
    handle->event.signaled = 0;
    spinlock_unlock(&handle->event.lock);
}

int _PalEventWait(PAL_HANDLE handle, uint64_t* timeout_us) {
    uint64_t start = vbs_time_ns() / 1000;
    while (1) {
        spinlock_lock(&handle->event.lock);
        if (handle->event.signaled) {
            if (handle->event.auto_clear)
                handle->event.signaled = 0;
            spinlock_unlock(&handle->event.lock);
            return 0;
        }
        spinlock_unlock(&handle->event.lock);
        if (!timeout_us) {
            log_error("vbs: waiting forever on an event with one enclave thread");
            return -PAL_ERROR_DENIED;
        }
        uint64_t now = vbs_time_ns() / 1000;
        if (now - start >= *timeout_us) {
            *timeout_us = 0;
            return -PAL_ERROR_TRYAGAIN;
        }
    }
}

static void event_destroy(PAL_HANDLE handle) {
    free(handle);
}

struct handle_ops g_event_ops = {
    .destroy = event_destroy,
};
