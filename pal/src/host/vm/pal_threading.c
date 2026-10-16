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
#if defined(__x86_64__) || defined(__aarch64__) /* the multiprocess frame pool */
    if (handle->private_frame)
        (void)memory_frame_free(handle->private_frame);
    else
#endif
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
    /* Interrupt the thread's blocking PAL call, if it is in one (an interruptible wait returns
     * -PAL_ERROR_INTERRUPTED and the LibOS handles the thread's pending signals), otherwise mark it
     * so that its next such wait does not sleep and its next user-mode timer tick delivers
     * PAL_EVENT_INTERRUPTED (pal_common_deliver_pending_interrupt()). History: the tick delivery
     * alone, without interruptible waits, was tried with the experimental multiprocess support and
     * stalled Mono (Bastion) after its first GC suspension (Documentation/devel/vm-multiproc/
     * FINDINGS.md); Documentation/devel/proton-vm/FINDINGS.md has this version's tests. */
    if (!thread || !thread->thread.kernel_thread)
        return -PAL_ERROR_INVAL;
    sched_thread_interrupt(thread->thread.kernel_thread);
    return 0;
#else
    /* Not implemented: the LibOS wakes a thread blocked in its own waits, and a thread notices a
     * new signal at its next system call. */
    __UNUSED(thread);
    return -PAL_ERROR_NOTIMPLEMENTED;
#endif
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
