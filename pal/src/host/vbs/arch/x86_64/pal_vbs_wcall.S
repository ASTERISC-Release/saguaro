/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/*
 * uint64_t pal_vbs_wcall(void* fn, uint64_t a0, uint64_t a1, uint64_t a2);
 *
 * Calls a service of the Windows enclave runtime (Windows x64 code) from the vbs PAL proper
 * (Documentation/devel/vbs-enclave/PLAN-x64.md, X4). The callee-saved registers of System V (rbx,
 * rbp, r12-r15) are callee-saved in Windows x64 too, and System V treats every other register as
 * clobbered, so the thunk only moves the arguments (rcx, rdx, r8), reserves the 32-byte shadow space
 * and aligns the stack. GS is the TEB, which the Windows code needs and no packed code writes.
 *
 * The FS base (the thread pointer of the Linux code) can come back from a W service changed: an
 * OCALL may leave a stale value, preemption resets it to 0 (FINDINGS-x64.md). It is saved before
 * and written back after every call.
 */

    .text
    .globl  pal_vbs_wcall
    .type   pal_vbs_wcall, @function
    .p2align 4
pal_vbs_wcall:
    pushq   %rbp
    movq    %rsp, %rbp
    pushq   %rbx
    rdfsbase %rbx
    andq    $-16, %rsp
    subq    $32, %rsp
    /* System V: rdi = fn, rsi = a0, rdx = a1, rcx = a2 -> Windows: rcx = a0, rdx = a1, r8 = a2 */
    movq    %rdi, %rax
    movq    %rcx, %r8
    movq    %rsi, %rcx
    callq   *%rax
    wrfsbase %rbx
    movq    -8(%rbp), %rbx
    leave
    retq
    .size   pal_vbs_wcall, .-pal_vbs_wcall

/*
 * noreturn void pal_vbs_run_on_stack(void (*fn)(void*), void* arg, void* stack_top);
 * Switches to a new (16-byte aligned) stack and calls fn(arg); fn does not return.
 */
    .globl  pal_vbs_run_on_stack
    .type   pal_vbs_run_on_stack, @function
    .p2align 4
pal_vbs_run_on_stack:
    andq    $-16, %rdx
    movq    %rdx, %rsp
    xorl    %ebp, %ebp
    movq    %rdi, %rax
    movq    %rsi, %rdi
    callq   *%rax
    ud2                     /* fn returned: must not happen */
    .size   pal_vbs_run_on_stack, .-pal_vbs_run_on_stack

    .section .note.GNU-stack,"",@progbits
