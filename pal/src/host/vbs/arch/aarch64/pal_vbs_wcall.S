/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 Gramine contributors */

/*
 * uint64_t pal_vbs_wcall(void* fn, uint64_t a0, uint64_t a1, uint64_t a2);
 *
 * Calls a service of the Windows enclave runtime (Windows ARM64 code) from the vbs PAL proper.
 * Windows code needs x18 = the enclave TEB; Linux code (above all an application's glibc, which is
 * not built with -ffixed-x18) may have used x18, so it is reloaded here before every call. Integer
 * arguments and the result use the same registers in both ABIs (PLAN.md D4).
 */

    .text
    .globl  pal_vbs_wcall
    .type   pal_vbs_wcall, %function
    .p2align 2
pal_vbs_wcall:
    stp     x29, x30, [sp, #-16]!
    mov     x29, sp
    adrp    x16, g_vbs_teb
    ldr     x18, [x16, :lo12:g_vbs_teb]
    mov     x16, x0
    mov     x0, x1
    mov     x1, x2
    mov     x2, x3
    blr     x16
    ldp     x29, x30, [sp], #16
    ret
    .size   pal_vbs_wcall, .-pal_vbs_wcall

    .section .note.GNU-stack,"",%progbits
