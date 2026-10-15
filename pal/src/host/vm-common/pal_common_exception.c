/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2024 Intel Corporation */

/*
 * This file contains handling of hardware exceptions (forwarding them to LibOS).
 */

#include "api.h"
#include "pal.h"
#include "pal_common.h"
#include "pal_internal.h"

#include "kernel_interrupts.h"
#include "kernel_memory.h"

/* fpregs is shallow copied by only setting a pointer */
static void isr_regs_to_pal_context(PAL_CONTEXT* context, struct isr_regs* regs,
                                    uint64_t faulted_addr) {
    context->r8  = regs->r8;
    context->r9  = regs->r9;
    context->r10 = regs->r10;
    context->r11 = regs->r11;
    context->r12 = regs->r12;
    context->r13 = regs->r13;
    context->r14 = regs->r14;
    context->r15 = regs->r15;
    context->rdi = regs->rdi;
    context->rsi = regs->rsi;
    context->rbp = regs->rbp;
    context->rbx = regs->rbx;
    context->rdx = regs->rdx;
    context->rax = regs->rax;
    context->rcx = regs->rcx;
    context->rsp = regs->rsp;
    context->rip = regs->rip;
    context->efl = regs->rflags;

    context->csgsfsss = 0; /* dummy */
    context->err      = 0; /* dummy */
    context->trapno   = 0; /* dummy */
    context->oldmask  = 0; /* dummy */
    context->cr2      = faulted_addr;

    context->mxcsr    = 0; /* dummy */
    context->fpcw     = 0; /* dummy */

    context->fpregs = regs->fpregs;
    context->is_fpregs_used = 1;
}

/* fpregs is shallow copied by only setting a pointer */
static void pal_context_to_isr_regs(struct isr_regs* regs, PAL_CONTEXT* context) {
    regs->r8  = context->r8;
    regs->r9  = context->r9;
    regs->r10 = context->r10;
    regs->r11 = context->r11;
    regs->r12 = context->r12;
    regs->r13 = context->r13;
    regs->r14 = context->r14;
    regs->r15 = context->r15;
    regs->rdi = context->rdi;
    regs->rsi = context->rsi;
    regs->rbp = context->rbp;
    regs->rbx = context->rbx;
    regs->rdx = context->rdx;
    regs->rax = context->rax;
    regs->rcx = context->rcx;
    regs->rsp = context->rsp;
    regs->rip = context->rip;
    regs->rflags = context->efl;

    regs->fpregs = context->fpregs;
}

#ifdef __x86_64__
/*
 * LibOS upcalls for events of a thread in user mode (a fault, or an interrupt request from
 * _PalThreadResume()) do not run inside the interrupt handler: all interrupts share one IST stack
 * and one per-CPU XSAVE area, so a fault inside the upcall (for example the first touch of a
 * demand-zero page while the LibOS writes a signal frame) would overwrite the interrupted state.
 * Instead, the user state is saved in the thread, the thread continues in kernel mode on its own
 * PAL stack (unused while it runs in user mode), runs the upcall there and returns to user mode
 * through the same path as a preempted thread.
 */
#include "kernel_thread.h"
#include "kernel_xsave.h"

int save_context_and_restore_next(uint64_t curr_gs_base, uint64_t next_gs_base,
                                  uint32_t* lock_to_unlock, int* clear_child_tid,
                                  uint32_t* critical_section_lock_to_unlock,
                                  void* scheduling_stack, void (*post_switch)(void*),
                                  void* post_switch_arg);
noreturn void isr_iret_to_userland(void);

static uint32_t g_upcall_dummy_lock;

static noreturn void run_current_thread_context(struct pal_tcb_vm* tcb) {
    save_context_and_restore_next(/*curr_gs_base=*/0, (uint64_t)tcb, /*lock_to_unlock=*/NULL,
                                  /*clear_child_tid=*/NULL, &g_upcall_dummy_lock,
                                  /*scheduling_stack=*/NULL, /*post_switch=*/NULL,
                                  /*post_switch_arg=*/NULL);
    __builtin_unreachable();
}

static noreturn void deferred_upcall_trampoline(void) {
    struct pal_tcb_vm* tcb = (struct pal_tcb_vm*)pal_get_tcb();
    struct thread* thread = &tcb->kernel_thread;

    struct isr_regs regs = thread->upcall_regs;
    regs.fpregs = thread->upcall_fpregs;
    uint64_t addr = thread->upcall_addr;
    pal_event_handler_t upcall = _PalGetExceptionHandler(thread->upcall_event);

    thread->context.user_rip = regs.rip;
    PAL_CONTEXT context;
    isr_regs_to_pal_context(&context, &regs, addr);
    if (upcall)
        (*upcall)(/*is_in_pal=*/false, addr, &context);
    pal_context_to_isr_regs(&regs, &context);

    /* return to user mode like a preempted thread: GPRs and FP state from the thread context,
     * RIP/RSP/RFLAGS through iretq from the pseudo stack */
    __asm__ volatile("cli" ::: "memory");
    memcpy(thread->context.fpregs, regs.fpregs, g_xsave_size);
    thread->context.r8  = regs.r8;
    thread->context.r9  = regs.r9;
    thread->context.r10 = regs.r10;
    thread->context.r11 = regs.r11;
    thread->context.r12 = regs.r12;
    thread->context.r13 = regs.r13;
    thread->context.r14 = regs.r14;
    thread->context.r15 = regs.r15;
    thread->context.rdi = regs.rdi;
    thread->context.rsi = regs.rsi;
    thread->context.rbp = regs.rbp;
    thread->context.rbx = regs.rbx;
    thread->context.rdx = regs.rdx;
    thread->context.rax = regs.rax;
    thread->context.rcx = regs.rcx;
    thread->irq_pseudo_stack.rip    = regs.rip;
    thread->irq_pseudo_stack.cs     = regs.cs;
    thread->irq_pseudo_stack.rflags = regs.rflags | 0x200;
    thread->irq_pseudo_stack.rsp    = regs.rsp;
    thread->irq_pseudo_stack.ss     = regs.ss;
    thread->context.rip    = (uint64_t)&isr_iret_to_userland;
    thread->context.rsp    = (uint64_t)&thread->irq_pseudo_stack;
    thread->context.rflags = 0x2; /* interrupts stay off until iretq */
    run_current_thread_context(tcb);
}

/* Called in the interrupt handler, for an interrupted user-mode context only. */
static noreturn void defer_user_upcall(enum pal_event event, uint64_t addr, struct isr_regs* regs) {
    struct pal_tcb_vm* tcb = (struct pal_tcb_vm*)pal_get_tcb();
    struct thread* thread = &tcb->kernel_thread;

    uintptr_t stack_top = (uintptr_t)tcb->thread_handle->thread.stack + THREAD_STACK_SIZE;
    void* fpregs = (void*)ALIGN_DOWN(stack_top - g_xsave_size, VM_XSAVE_ALIGN);
    memcpy(fpregs, regs->fpregs, g_xsave_size);

    thread->upcall_regs   = *regs;
    thread->upcall_fpregs = fpregs;
    thread->upcall_addr   = addr;
    thread->upcall_event  = event;
    thread->context.rip    = (uint64_t)&deferred_upcall_trampoline;
    thread->context.rsp    = ALIGN_DOWN((uintptr_t)fpregs - 64, 16) - 8; /* as after a call */
    thread->context.rflags = 0x202;
    run_current_thread_context(tcb);
}

static bool is_user_mode(struct isr_regs* regs) {
    return (regs->cs & 3) != 0;
}
#endif

/* A CPU exception of the application becomes a LibOS event: a memory fault (#PF, #GP), an illegal
 * instruction (#UD) or an arithmetic error (#DE, #MF, #XM). `addr` is the faulting address for a
 * memory fault and the instruction's address otherwise, as the Linux PAL reports si_addr. Returns
 * -PAL_ERROR_DENIED when the LibOS cannot take it (no handler yet, or the fault is the PAL's own). */
int pal_common_perform_event_handling(enum pal_event event, uint64_t addr, struct isr_regs* regs) {
    pal_event_handler_t upcall = _PalGetExceptionHandler(event);
    if (!upcall)
        return -PAL_ERROR_DENIED;

    struct pal_tcb_vm* curr_tcb = (struct pal_tcb_vm*)pal_get_tcb();
    if (!curr_tcb)
        return -PAL_ERROR_DENIED;

#if defined(PAL_HOST_VM) && defined(__x86_64__)
    /* A fault in the PAL's own code is a PAL bug, which the LibOS cannot handle: it would take it
     * for a fault of the application, and in a child process whose LibOS is not running yet its
     * handler faults again, forever, while the PAL holds its locks. The caller panics instead,
     * with the fault's address. */
    extern char __text_start[], __text_end[];
    if (!is_user_mode(regs) && regs->rip >= (uintptr_t)__text_start &&
            regs->rip < (uintptr_t)__text_end)
        return -PAL_ERROR_DENIED;
#endif

#ifdef __x86_64__
    /* with the experimental multiprocess support (demand-zero pages may fault in the upcall) */
    if (is_user_mode(regs) && curr_tcb->thread_handle && memory_frame_pool_active())
        defer_user_upcall(event, addr, regs);
#endif

    /* RIP in the isr_regs is the actual user RIP */
    curr_tcb->kernel_thread.context.user_rip = regs->rip;

    PAL_CONTEXT context;
    isr_regs_to_pal_context(&context, regs, addr);
    (*upcall)(/*is_in_pal=*/false, addr, &context);
    pal_context_to_isr_regs(regs, &context);

    return 0;
}

int pal_common_perform_memfault_handling(uint64_t faulted_addr, struct isr_regs* regs) {
    return pal_common_perform_event_handling(PAL_EVENT_MEMFAULT, faulted_addr, regs);
}

#ifdef __x86_64__
/* Called by the timer interrupt that preempts a thread in user mode: if another thread asked to
 * interrupt it (_PalThreadResume(), used by the LibOS to make a thread notice a new signal), let
 * the LibOS handle its pending signals now, with the interrupted user context, as on the Linux PAL
 * where the host signal interrupts the thread. */
void pal_common_deliver_pending_interrupt(struct isr_regs* regs) {
    struct pal_tcb_vm* curr_tcb = (struct pal_tcb_vm*)pal_get_tcb();
    if (!curr_tcb || !__atomic_exchange_n(&curr_tcb->kernel_thread.resume_pending, false,
                                          __ATOMIC_ACQ_REL))
        return;

    if (!_PalGetExceptionHandler(PAL_EVENT_INTERRUPTED) || !curr_tcb->thread_handle ||
            !is_user_mode(regs))
        return;
    defer_user_upcall(PAL_EVENT_INTERRUPTED, /*addr=*/0, regs);
}
#endif
