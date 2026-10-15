/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 Gramine contributors */

/* Events of the vbs PAL proper: a 32-bit word, waited on with WaitOnAddress (W's wait service) and
 * woken with WakeByAddressAll, which work across enclave threads (phase 0, step e). */

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
    __atomic_store_n(&handle->event.signaled, 1, __ATOMIC_RELEASE);
    vbs_wake(&handle->event.signaled, /*all=*/true);
}

void _PalEventClear(PAL_HANDLE handle) {
    __atomic_store_n(&handle->event.signaled, 0, __ATOMIC_RELEASE);
}

int _PalEventWait(PAL_HANDLE handle, uint64_t* timeout_us) {
    uint64_t start = timeout_us ? vbs_time_ns() : 0;
    while (1) {
        if (handle->event.auto_clear) {
            uint32_t one = 1;
            if (__atomic_compare_exchange_n(&handle->event.signaled, &one, 0, false,
                                            __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
                break;
        } else if (__atomic_load_n(&handle->event.signaled, __ATOMIC_ACQUIRE)) {
            break;
        }
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
        if (left_ns < VBS_SHORT_WAIT_NS) {
            /* a WaitOnAddress timeout is rounded up to the system timer tick (measured: ~15.6 ms,
             * FINDINGS.md); a short wait polls the clock instead */
            continue;
        }
        vbs_wait(&handle->event.signaled, 0, left_ns);
    }
    if (timeout_us) {
        uint64_t spent_us = (vbs_time_ns() - start) / 1000;
        *timeout_us = spent_us >= *timeout_us ? 0 : *timeout_us - spent_us;
    }
    return 0;
}

static void event_destroy(PAL_HANDLE handle) {
    free(handle);
}

struct handle_ops g_event_ops = {
    .destroy = event_destroy,
};
