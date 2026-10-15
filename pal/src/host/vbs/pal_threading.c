/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 Gramine contributors */

/* Threads of the vbs PAL proper: one enclave thread for now (more threads enter the enclave through
 * the host in phase 3; the enclave's thread count is fixed at signing). */

#include "api.h"
#include "pal.h"
#include "pal_error.h"
#include "pal_internal.h"
#include "pal_vbs.h"

int _PalThreadCreate(PAL_HANDLE* handle, int (*callback)(void*), void* param) {
    __UNUSED(handle);
    __UNUSED(callback);
    __UNUSED(param);
    return -PAL_ERROR_NOTIMPLEMENTED;
}

void _PalThreadYieldExecution(void) {
    /* one thread: nothing to yield to */
}

noreturn void _PalThreadExit(int* clear_child_tid) {
    __UNUSED(clear_child_tid);
    /* the only thread exits: the process ends */
    vbs_exit(0);
}

int _PalThreadResume(PAL_HANDLE thread_handle) {
    __UNUSED(thread_handle);
    return -PAL_ERROR_NOTIMPLEMENTED;
}

int _PalThreadSetCpuAffinity(PAL_HANDLE thread, unsigned long* cpu_mask, size_t cpu_mask_len) {
    __UNUSED(thread);
    __UNUSED(cpu_mask);
    __UNUSED(cpu_mask_len);
    return 0;
}

int _PalThreadGetCpuAffinity(PAL_HANDLE thread, unsigned long* cpu_mask, size_t cpu_mask_len) {
    __UNUSED(thread);
    if (cpu_mask_len < 1)
        return -PAL_ERROR_INVAL;
    memset(cpu_mask, 0, cpu_mask_len * sizeof(*cpu_mask));
    cpu_mask[0] = 1;
    return 0;
}

struct handle_ops g_thread_ops = {
    /* nothing */
};
