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

#include "kernel_memory.h"
#include "kernel_multicore.h"
#include "kernel_sched.h"
#include "kernel_thread.h"
#include "kernel_xsave.h"

static struct pal_tcb_vm g_dummy_tcb = {
    .common.stack_protector_canary = STACK_PROTECTOR_CANARY_DEFAULT
};

/*
 * aarch64 backend of pal_get_tcb() (see pal/include/arch/aarch64/pal_arch.h): on x86_64 that
 * is one `movq %gs:PAL_TCB.common.self`, where the GS base holds &pal_tcb_vm (written by
 * save_context_and_restore_next) and the `self` slot points at the embedded common PAL_TCB.
 * On aarch64 the GS-base role is served by per_cpu_data->cur_tcb (kernel_multicore.h) holding
 * exactly the same &pal_tcb_vm value, so we resolve the very same `common` member through it.
 * During early boot and thread death, cur_tcb is the dummy TCB below (set_dummy_gs_base()),
 * which keeps GCC's stack-protector lookups valid -- exactly like the x86 dummy GS install.
 */
PAL_TCB* pal_get_tcb_vm(void) {
    struct pal_tcb_vm* tcb = (struct pal_tcb_vm*)get_per_cpu_data()->cur_tcb;
    return &tcb->common;
}

static int assign_new_tid(void) {
    static int tid = 0;
    return __atomic_add_fetch(&tid, 1, __ATOMIC_SEQ_CST);
}

int pal_common_thread_create(struct pal_handle** handle, int (*callback)(void*),
                             const void* param) {
    struct pal_tcb_vm* curr_tcb = (struct pal_tcb_vm*)pal_get_tcb();
    struct memory_address_space* address_space = curr_tcb
            ? curr_tcb->kernel_thread.address_space
            : memory_address_space_boot();
    if (!address_space)
        address_space = memory_address_space_boot();
    return pal_common_thread_create_with_address_space(handle, callback, param,
                                                       address_space);
}

/* (x86 vm-common/pal_common_threading.c) A thread created in another process's root (the first
 * thread of a child process) gets its handle in a pool frame: the handle must stay reachable from
 * every root, and the creating process may exit first. */
int pal_common_thread_create_with_address_space(struct pal_handle** handle,
                                               int (*callback)(void*), const void* param,
                                               struct memory_address_space* address_space) {
    if (!address_space || !address_space->root || !address_space->root_phys ||
            !IS_ALIGNED_POW2(address_space->root_phys, PAGE_SIZE))
        return -PAL_ERROR_INVAL;

    /*
     * Stack layout for the new thread looks like this:
     *
     *                   +-------------------+
     *                   |  PAL TCB          | sizeof(struct pal_tcb_vm)
     *          tcb +--> +-------------------+
     *                   |  alternate stack  | ALT_STACK_SIZE - sizeof(struct pal_tcb_vm)
     *                   +-------------------+
     *                   |  normal stack     | THREAD_STACK_SIZE
     *       stack  +--> +-------------------+
     */

    struct pal_tcb_vm* creator_tcb = (struct pal_tcb_vm*)pal_get_tcb();
    struct memory_address_space* current_space = creator_tcb
            ? creator_tcb->kernel_thread.address_space
            : NULL;
    if (!current_space)
        current_space = memory_address_space_boot();
    struct pal_handle* thread_handle = NULL;
    uintptr_t handle_frame = 0;
    if (address_space != current_space) {
        int ret = memory_frame_alloc(&handle_frame);
        if (ret < 0)
            return ret;
        thread_handle = memory_frame_pointer(handle_frame);
        memset(thread_handle, 0, PAGE_SIZE);
        thread_handle->private_frame = handle_frame;
    } else {
        thread_handle = calloc(1, sizeof(*thread_handle));
        if (!thread_handle)
            return -PAL_ERROR_NOMEM;
    }

    void* stack;
    void* fpregs;
    int ret = thread_get_stack_and_fpregs(&stack, &fpregs);
    if (ret < 0) {
        if (handle_frame)
            memory_frame_free(handle_frame);
        else
            free(thread_handle);
        return ret;
    }

    /* Every root must be able to traverse the shared scheduler list, including the TCB node for a
     * thread created after that root was cloned. Stacks allocated from the frame alias (every
     * stack once the frame pool exists: the PAL heap then comes from the pool) are already mapped
     * identically in every root. */
    if (memory_frame_pool_active() && !memory_frame_is_alias_pointer(stack)) {
        size_t stack_size = THREAD_STACK_SIZE + ALT_STACK_SIZE + g_xsave_size + VM_XSAVE_ALIGN;
        uintptr_t stack_start = ALIGN_DOWN((uintptr_t)stack, PAGE_SIZE);
        uintptr_t stack_end = ALIGN_UP((uintptr_t)stack + stack_size, PAGE_SIZE);
        ret = sched_thread_share_stack(current_space, address_space, stack_start,
                                       stack_end - stack_start);
        if (ret < 0) {
            thread_release_stack(stack);
            if (handle_frame)
                memory_frame_free(handle_frame);
            else
                free(thread_handle);
            return ret;
        }
    }

    /* init TCB in the highest part of the allocated stack region */
    struct pal_tcb_vm* tcb = stack + THREAD_STACK_SIZE + ALT_STACK_SIZE - sizeof(*tcb);
    thread_setup(&tcb->kernel_thread, fpregs, stack, callback, param);
    tcb->kernel_thread.address_space = address_space;

    thread_handle->hdr.type = PAL_TYPE_THREAD;
    thread_handle->thread.tid = assign_new_tid();
    thread_handle->thread.stack = stack;
    thread_handle->thread.kernel_thread = &tcb->kernel_thread;

    tcb->common.self   = &tcb->common;
    tcb->thread_handle = thread_handle;

    /* thread_handle may be created via libos_syscall_clone(); in this case the newly created
     * thread_handle jumps to the `eret`-based return-to-user trampoline (x86 `sysret_asm`), which
     * expects user_rip to point to actual EL0 rip */
    struct pal_tcb_vm* curr_tcb = creator_tcb;
    if (curr_tcb) {
        tcb->kernel_thread.context.user_rip = curr_tcb->kernel_thread.context.user_rip;

        /* Linux clone() semantics: the child resumes right after the parent's `svc` with a
         * *copy of the parent's pt_regs* (only x0 differs: 0 for the child). On aarch64 glibc's
         * clone keeps the thread fn in x10 and its arg in x12 *across the svc* (clone+0x58:
         * `mov x0, x12; blr x10`) -- x86's glibc instead stashes them on the child's stack, so
         * x86's sysret_asm trampoline (which restores no GPRs) suffices there but aarch64's
         * cannot: an unseeded child returns through pal_syscall_return with calloc'd-zero
         * scratch slots and jumps through x10 = 0. The creator's scratch slots hold exactly the
         * parent snapshot the child must inherit (taken at el0_sync_entry, never written by the
         * EL1 dispatcher), including the process-wide x18 pinning. Non-clone PAL threads get a
         * meaningless seed -- they never eret to userland, so it is inert. */
        memcpy(tcb->kernel_thread.context.syscall_scratch,
               curr_tcb->kernel_thread.context.syscall_scratch,
               sizeof(tcb->kernel_thread.context.syscall_scratch));
    }

    sched_thread_add(&tcb->kernel_thread);

    *handle = thread_handle;
    return 0;
}

noreturn void pal_common_thread_exit(int* clear_child_tid) {
    struct pal_tcb_vm* curr_tcb = (struct pal_tcb_vm*)pal_get_tcb();
    struct pal_handle* thread_handle = curr_tcb->thread_handle;
    assert(thread_handle);

    /* get this thread_handle off the schedulable list */
    sched_thread_remove(&curr_tcb->kernel_thread);

	thread_free_stack_and_die(thread_handle->thread.stack, clear_child_tid);
    __builtin_unreachable();
}

int pal_common_thread_set_cpu_affinity(struct pal_handle* thread, unsigned long* cpu_mask,
                                       size_t cpu_mask_len) {
    sched_thread_set_cpu_affinity(thread->thread.kernel_thread, cpu_mask, cpu_mask_len);
    return 0;
}

int pal_common_thread_get_cpu_affinity(struct pal_handle* thread, unsigned long* cpu_mask,
                                       size_t cpu_mask_len) {
    __UNUSED(thread);
    assert(g_num_cpus >= 1 && g_num_cpus <= MAX_NUM_CPUS);

    static bool called = false;
    if (__atomic_exchange_n(&called, true, __ATOMIC_ACQUIRE)) {
        /* LibOS must call this API only once (at init) */
        log_error("Get CPU affinity called twice?! Bug in LibOS.");
        BUG();
    }

    if (cpu_mask_len * BITS_IN_TYPE(*cpu_mask) > MAX_NUM_CPUS) {
        log_error("Get CPU affinity: too many CPUs requested in CPU mask.");
        return -PAL_ERROR_INVAL;
    }

    if (cpu_mask_len * BITS_IN_TYPE(*cpu_mask) < g_num_cpus) {
        log_error("Get CPU affinity: CPU mask cannot fit all CPUs.");
        return -PAL_ERROR_INVAL;
    }

    for (size_t i = 0; i < g_num_cpus; i++) {
        cpu_mask[i / BITS_IN_TYPE(*cpu_mask)] |= 1UL << (i % BITS_IN_TYPE(*cpu_mask));
    }
    return 0;
}

struct thread* get_thread_ptr(uintptr_t curr_gs_base) {
    struct pal_tcb_vm* curr_tcb = (struct pal_tcb_vm*)curr_gs_base;
    return &curr_tcb->kernel_thread;
}

uintptr_t get_gs_base(struct thread* next_thread) {
    return (uintptr_t)next_thread - offsetof(struct pal_tcb_vm, kernel_thread);
}

/* In cases of (1) bootstrapping the VM and (2) receiving interrupts while current thread is
 * terminating, "current TCB" is meaningless, but C code compiled with -fstack-protector and the
 * svc dispatcher still want a well-formed TCB pointer to exist, so let's install a dummy TCB
 * into the per-CPU current-TCB slot (the aarch64 stand-in for x86's GS base MSR; the stack
 * protector itself uses the global-canary scheme here, so this is for dispatcher bookkeeping
 * rather than for the canary load). See arch/aarch64/kernel_multicore.h for the slot design. */
__attribute_no_stack_protector
__attribute_no_sanitize_address
void set_dummy_gs_base(void) {
    get_per_cpu_data()->cur_tcb = &g_dummy_tcb;
    /* no separate EL0-readable mirror to keep in sync: TPIDRRO_EL0 already points at this
     * exact cur_tcb slot (installed per CPU at boot), so the store above is simultaneously
     * the kernel-side and the EL0-side "current TCB" update */
}

__attribute_no_stack_protector
uintptr_t replace_with_null_if_dummy_gs_base(uintptr_t gs_base) {
    return gs_base == (uintptr_t)&g_dummy_tcb ? 0 : gs_base;
}
