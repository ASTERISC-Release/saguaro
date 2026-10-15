/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 Gramine contributors */

/*
 * AArch64 boundary thunks for the VBS-enclave PAL (phase 1 skeleton). GAS syntax; assembled on
 * Linux by clang's integrated assembler (--target=aarch64-pc-windows-msvc) or on Windows by
 * armasm64 from the .asm twin. See Documentation/devel/vbs-enclave/PLAN.md D4.
 *
 * x18 is the enclave TEB on Windows ARM64 (phase 0, step d). Linux System V/AAPCS64 code treats
 * x18 as a scratch register and will clobber it, which breaks the enclave runtime and SEH dispatch.
 * pal_vbs_enter_sysv() therefore saves x18 across the call and restores it before returning to
 * Windows code. In phase 1 the "System V" callee is a local test function; in phase 2 it is the
 * packed glibc/LibOS entry.
 */

    .text

/* uint64_t pal_vbs_read_x18(void); */
    .globl  pal_vbs_read_x18
    .p2align 2
pal_vbs_read_x18:
    mov     x0, x18
    ret

/*
 * uint64_t pal_vbs_enter_sysv(void* fn, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3);
 *
 * Windows ARM64 ABI passes fn=x0, a0=x1, a1=x2, a2=x3, a3=x4. We call fn(a0,a1,a2,a3) with the
 * arguments in x0..x3 (shared by AAPCS64 and Windows ARM64 for the first integer args), saving
 * and restoring x18 (the TEB) and the link register across the call.
 */
    .globl  pal_vbs_enter_sysv
    .p2align 2
pal_vbs_enter_sysv:
    stp     x29, x30, [sp, #-32]!
    mov     x29, sp
    str     x18, [sp, #16]          /* preserve the enclave TEB */
    mov     x16, x0                 /* fn */
    mov     x0, x1                  /* a0 */
    mov     x1, x2                  /* a1 */
    mov     x2, x3                  /* a2 */
    mov     x3, x4                  /* a3 */
    blr     x16
    ldr     x18, [sp, #16]          /* restore the enclave TEB, whatever the callee did */
    ldp     x29, x30, [sp], #32
    ret

/* int64_t pal_vbs_brk0(void); raises a breakpoint (caught by SEH), then returns 0 if resumed. */
    .globl  pal_vbs_brk0
    .p2align 2
pal_vbs_brk0:
    brk     #0
    mov     x0, #0
    ret
