/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 Gramine contributors */

/*
 * Threads of the vbs PAL proper. A new thread is a host thread that enters the enclave through the
 * E_ThreadStart ECALL (W's thread_create service); the number of threads inside the enclave at once
 * is fixed when the image is signed (vbs.max_threads) and when the host initialises the enclave
 * (ThreadCount), so the PAL refuses a thread beyond that count instead of letting it wait for a
 * free slot forever. Each thread runs on a PAL stack of its own; x18 is its TEB throughout (no
 * packed code writes x18).
 */

#include "api.h"
#include "pal.h"
#include "pal_error.h"
#include "pal_internal.h"
#include "pal_vbs.h"

#define VBS_THREAD_STACK_SIZE (256 * 1024)

static uint32_t g_live_threads = 1; /* the first thread */

/* switches to `stack_top` and calls fn(arg) there; does not return (pal_vbs_wcall.S) */
noreturn void pal_vbs_run_on_stack(void (*fn)(void*), void* arg, void* stack_top);

static noreturn void thread_main(void* arg) {
    PAL_VBS_TCB* tcb = arg;
    tcb->callback(tcb->param);
    _PalThreadExit(/*clear_child_tid=*/NULL);
}

/* entered by W on the new thread (W's stack): entry(arg = tcb, ctx = W's context of the thread) */
noreturn void pal_vbs_thread_entry(PAL_VBS_TCB* tcb, void* w_ctx);
__attribute_no_stack_protector
noreturn void pal_vbs_thread_entry(PAL_VBS_TCB* tcb, void* w_ctx) {
    tcb->w_ctx = w_ctx;
    pal_tcb_arch_set_stack_canary(&tcb->common, STACK_PROTECTOR_CANARY_DEFAULT);
    pal_set_tcb(&tcb->common);
    uint64_t canary;
    if (vbs_random(&canary, sizeof(canary)) == 0)
        pal_tcb_arch_set_stack_canary(&tcb->common, canary);
    pal_vbs_run_on_stack(thread_main, tcb, (char*)tcb->stack + VBS_THREAD_STACK_SIZE);
}

int _PalThreadCreate(PAL_HANDLE* handle, int (*callback)(void*), void* param) {
    uint32_t max = g_vbs->thread_count ? (uint32_t)g_vbs->thread_count : 1;
    if (__atomic_add_fetch(&g_live_threads, 1, __ATOMIC_ACQ_REL) > max) {
        __atomic_sub_fetch(&g_live_threads, 1, __ATOMIC_ACQ_REL);
        log_warning("vbs: no free enclave thread (the image allows %u at once)", max);
        return -PAL_ERROR_NOMEM;
    }

    PAL_HANDLE hdl = calloc(1, HANDLE_SIZE(thread));
    PAL_VBS_TCB* tcb = calloc(1, sizeof(*tcb));
    void* stack = malloc(VBS_THREAD_STACK_SIZE);
    if (!hdl || !tcb || !stack) {
        free(hdl);
        free(tcb);
        free(stack);
        __atomic_sub_fetch(&g_live_threads, 1, __ATOMIC_ACQ_REL);
        return -PAL_ERROR_NOMEM;
    }
    static uint32_t next_tid = 1;
    init_handle_hdr(hdl, PAL_TYPE_THREAD);
    hdl->thread.tid = __atomic_add_fetch(&next_tid, 1, __ATOMIC_ACQ_REL);
    tcb->common.self = &tcb->common;
    tcb->handle = hdl;
    tcb->stack = stack;
    tcb->callback = callback;
    tcb->param = param;

    int64_t ret = vbs_thread_create((void*)pal_vbs_thread_entry, tcb);
    if (ret < 0) {
        log_warning("vbs: starting an enclave thread failed: %ld", ret);
        free(hdl);
        free(tcb);
        free(stack);
        __atomic_sub_fetch(&g_live_threads, 1, __ATOMIC_ACQ_REL);
        return -PAL_ERROR_NOMEM;
    }
    *handle = hdl;
    return 0;
}

void _PalThreadYieldExecution(void) {
    /* sleep 0 ms on a private word: gives the processor away briefly */
    static uint32_t never;
    vbs_wait(&never, 0, 0);
}

noreturn void vbs_thread_exit(void* ctx, int* clear_tid) {
    pal_vbs_wcall(g_vbs->thread_exit, (uint64_t)ctx, (uint64_t)clear_tid, 0);
    __builtin_unreachable();
}

noreturn void _PalThreadExit(int* clear_child_tid) {
    __atomic_sub_fetch(&g_live_threads, 1, __ATOMIC_ACQ_REL);
    /* W finds the thread by its TEB: the LibOS has already dropped the thread's TLS registration,
     * so pal_get_tcb() may not find this thread's TCB any more. The TCB and stack are not freed:
     * the thread runs on that stack until W has switched back to its own (TODO: free them later) */
    vbs_thread_exit(NULL, clear_child_tid);
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
    size_t n = g_pal_public_state.topo_info.threads_cnt;
    for (size_t i = 0; i < n && i / (8 * sizeof(*cpu_mask)) < cpu_mask_len; i++)
        cpu_mask[i / (8 * sizeof(*cpu_mask))] |= 1ul << (i % (8 * sizeof(*cpu_mask)));
    return 0;
}

struct handle_ops g_thread_ops = {
    /* nothing */
};
