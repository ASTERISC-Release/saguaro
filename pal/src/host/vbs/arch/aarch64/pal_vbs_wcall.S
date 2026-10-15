/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/*
 * uint64_t pal_vbs_wcall(void* fn, uint64_t a0, uint64_t a1, uint64_t a2);
 *
 * Calls a service of the Windows enclave runtime (Windows ARM64 code) from the vbs PAL proper.
 * Windows code needs x18 = the TEB of the current thread. No packed code ever writes x18 (all of it
 * is built with -ffixed-x18 and pack.py refuses objects that use x18), and the kernel reloads x18
 * with the TEB whenever a preempted enclave thread resumes, so x18 already holds the right TEB on
 * every thread: the call needs no fix-up. Integer arguments and the result use the same registers
 * in both ABIs (PLAN.md D4).
 *
 * (The thread pointer is not TPIDR_EL0 in packed code: pack.py rewrites it to the TEB, which needs
 * no saving here; see FINDINGS.md.)
 */

    .text
    .globl  pal_vbs_wcall
    .type   pal_vbs_wcall, %function
    .p2align 2
pal_vbs_wcall:
    stp     x29, x30, [sp, #-16]!
    mov     x29, sp
    mov     x16, x0
    mov     x0, x1
    mov     x1, x2
    mov     x2, x3
    blr     x16
    ldp     x29, x30, [sp], #16
    ret
    .size   pal_vbs_wcall, .-pal_vbs_wcall

    .section .note.GNU-stack,"",%progbits

/*
 * noreturn void pal_vbs_run_on_stack(void (*fn)(void*), void* arg, void* stack_top);
 * Switches to a new (16-byte aligned) stack and calls fn(arg); fn does not return.
 */
    .text
    .globl  pal_vbs_run_on_stack
    .type   pal_vbs_run_on_stack, %function
    .p2align 2
pal_vbs_run_on_stack:
    and     x2, x2, #~15
    mov     sp, x2
    mov     x29, #0
    mov     x30, #0
    mov     x16, x0
    mov     x0, x1
    blr     x16
    brk     #0x5654         /* fn returned: must not happen */
    .size   pal_vbs_run_on_stack, .-pal_vbs_run_on_stack
