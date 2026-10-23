/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2023 Intel Corporation */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/*
 * aarch64 counterpart of vm-common/generated_offsets.c: compile-time offsets consumed by
 * arch/aarch64/kernel_context.S (and later by the memory/gic code). See
 * common/include/generated_offsets_build.h for the generation mechanism.
 *
 * Register offsets for x86's 19 named GPR fields (PAL_TCB_VM_CONTEXT_RAX etc.) have no meaning
 * here: aarch64 `struct thread_context` stores GPRs as an array `regs[31]`, so the asm computes
 * `PAL_TCB_VM_CONTEXT_REGS + 8 * <regno>` directly.
 */

#include "generated_offsets_build.h"
#include "pal.h"
#include "pal_common.h"

#include "kernel_multicore.h"
#include "kernel_sched.h"
#include "kernel_syscalls.h"
#include "kernel_thread.h"
#include "kernel_xsave.h"

const char* generated_offsets_name = "PAL_VM";

const struct generated_offset generated_offsets[] = {
    DEFINE(SYSCALL_RT_SIGRETURN, SYSCALL_RT_SIGRETURN),

    DEFINE(VM_XSAVE_ALIGN, VM_XSAVE_ALIGN),

    /* multicore constants */
    DEFINE(MAX_NUM_CPUS, MAX_NUM_CPUS),

    /* struct per_cpu_data (reachable in asm via `mrs tpidr_el1`) */
    OFFSET(PER_CPU_DATA_CPU_ID, per_cpu_data, cpu_id),
    DEFINE(PER_CPU_DATA_SIZE, sizeof(struct per_cpu_data)),
    OFFSET(PER_CPU_DATA_INTERRUPT_STACK, per_cpu_data, interrupt_stack),
    OFFSET(PER_CPU_DATA_INTERRUPT_FPSIMD_AREA, per_cpu_data, interrupt_fpsimd_area),
    OFFSET(PER_CPU_DATA_SCHEDULING_STACK, per_cpu_data, scheduling_stack),
    OFFSET(PER_CPU_DATA_IDLE_THREAD, per_cpu_data, idle_thread),
    OFFSET(PER_CPU_DATA_BOTTOMHALVES_THREAD, per_cpu_data, bottomhalves_thread),
    /* the "GS base" role: current thread TCB slot (kernel_events.S keeps it in sync with each
     * context switch, exactly like x86_64 wrmsr(MSR_IA32_GS_BASE)) */
    OFFSET(PER_CPU_DATA_CUR_TCB, per_cpu_data, cur_tcb),
    OFFSET(PER_CPU_DATA_DBG_IRQ_TRACE, per_cpu_data, dbg_irq_trace),

    /* struct pal_tcb_vm: entry to the LibOS syscall dispatcher */
    OFFSET(PAL_TCB_LIBOS, pal_tcb_vm, common.libos_tcb),
    OFFSET(PAL_TCB_VM_KERNEL_THREAD, pal_tcb_vm, kernel_thread),
    OFFSET(PAL_TCB_VM_THREAD_HANDLE, pal_tcb_vm, thread_handle),

    /* struct thread_context: GPRs at PAL_CONTEXT-compatible offsets */
    OFFSET(PAL_TCB_VM_CONTEXT_REGS, pal_tcb_vm, kernel_thread.context.regs),
    OFFSET(PAL_TCB_VM_CONTEXT_SP, pal_tcb_vm, kernel_thread.context.sp),
    OFFSET(PAL_TCB_VM_CONTEXT_PC, pal_tcb_vm, kernel_thread.context.pc),
    OFFSET(PAL_TCB_VM_CONTEXT_PSTATE, pal_tcb_vm, kernel_thread.context.pstate),
    OFFSET(PAL_TCB_VM_CONTEXT_FAULT_ADDRESS, pal_tcb_vm, kernel_thread.context.fault_address),
    OFFSET(PAL_TCB_VM_CONTEXT_ESR, pal_tcb_vm, kernel_thread.context.esr),
    OFFSET(PAL_TCB_VM_CONTEXT_OLDMASK, pal_tcb_vm, kernel_thread.context.oldmask),
    OFFSET(PAL_TCB_VM_CONTEXT_FPREGS, pal_tcb_vm, kernel_thread.context.fpregs),
    OFFSET(PAL_TCB_VM_CONTEXT_FPSR, pal_tcb_vm, kernel_thread.context.fpsr),
    OFFSET(PAL_TCB_VM_CONTEXT_FPCR, pal_tcb_vm, kernel_thread.context.fpcr),
    OFFSET(PAL_TCB_VM_CONTEXT_IS_FPREGS_USED, pal_tcb_vm, kernel_thread.context.is_fpregs_used),
    OFFSET(PAL_TCB_VM_CONTEXT_ORIG_X0, pal_tcb_vm, kernel_thread.context.orig_x0),
    /* svc resume address (x86 user_rip): where to return in EL0 after a syscall */
    OFFSET(PAL_TCB_VM_USER_RIP, pal_tcb_vm, kernel_thread.context.user_rip),
    /* TPIDR_EL0 value belonging to this thread (x86 user_fsbase) */
    OFFSET(PAL_TCB_VM_CONTEXT_USER_TLS, pal_tcb_vm, kernel_thread.context.user_tls),
    /* syscall snapshot of x1..x15/x18 (Linux syscall ABI preserves them; see kernel_thread.h) */
    OFFSET(PAL_TCB_VM_SYSCALL_SCRATCH, pal_tcb_vm, kernel_thread.context.syscall_scratch),

    /* struct thread: the eret frames and sigreturn scratch, based at pal_tcb_vm
     * (`struct thread kernel_thread` is not its first member, so OFFSET(thread, ...) values
     * would be wrong for the asm, which reaches them through cur_tcb) */
    OFFSET(PAL_TCB_VM_IRQ_PSEUDO_STACK, pal_tcb_vm, kernel_thread.irq_pseudo_stack),
    OFFSET(PAL_TCB_VM_SIGRETURN_USER_RSP, pal_tcb_vm, kernel_thread.sigreturn_user_rsp),
    OFFSET(PAL_TCB_VM_SIGRETURN_USER_RAX, pal_tcb_vm, kernel_thread.sigreturn_user_rax),
    OFFSET(PAL_TCB_VM_SIGRETURN_PSEUDO_RSP, pal_tcb_vm, kernel_thread.sigreturn_pseudo_rsp),
    OFFSET(PAL_TCB_VM_SIGRETURN_PSEUDO_STACK, pal_tcb_vm, kernel_thread.sigreturn_pseudo_stack),

    OFFSET(THREAD_STATE, thread, state),
    OFFSET(THREAD_BLOCKED_ON, thread, blocked_on),

    OFFSET_END
};
