/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2023 Intel Corporation */

/*
 * This file contains APIs to create, exit and yield a thread.
 */

#include "api.h"
#include "pal.h"
#include "pal_common.h"
#include "pal_error.h"
#include "pal_internal.h"

#include "kernel_sched.h"
#include "kernel_memory.h"
#include "kernel_thread.h"

static void thread_destroy(PAL_HANDLE handle) {
    if (handle->private_frame)
        (void)memory_frame_free(handle->private_frame);
    else
        free(handle);
}

int _PalThreadCreate(struct pal_handle** handle, int (*callback)(void*), void* param) {
    return pal_common_thread_create(handle, callback, param);
}

noreturn void _PalThreadExit(int* clear_child_tid) {
    pal_common_thread_exit(clear_child_tid);
}

void _PalThreadYieldExecution(void) {
    sched_thread(/*lock_to_unlock=*/NULL, /*clear_child_tid=*/NULL);
}

int _PalThreadResume(struct pal_handle* thread) {
#ifdef __x86_64__
    /* Experimental multiprocess support only, for now: the default VM keeps its earlier behavior.
     * A thread blocked in the LibOS is woken by the LibOS itself; one running or preempted in user
     * mode handles its signals at its next timer tick in user mode. */
    if (memory_frame_pool_active()) {
        __atomic_store_n(&thread->thread.kernel_thread->resume_pending, true, __ATOMIC_RELEASE);
        return 0;
    }
#endif
    __UNUSED(thread);
    return -PAL_ERROR_NOTIMPLEMENTED;
}

int _PalThreadSetCpuAffinity(struct pal_handle* thread, unsigned long* cpu_mask,
                             size_t cpu_mask_len) {
    return pal_common_thread_set_cpu_affinity(thread, cpu_mask, cpu_mask_len);
}

int _PalThreadGetCpuAffinity(struct pal_handle* thread, unsigned long* cpu_mask,
                             size_t cpu_mask_len) {
    return pal_common_thread_get_cpu_affinity(thread, cpu_mask, cpu_mask_len);
}

struct handle_ops g_thread_ops = {
    .destroy = &thread_destroy,
};
