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

/*
 * Boot context (phase 2): E_PalMain saves the Windows-side callee-saved state before it enters the
 * vbs PAL proper, which never returns; the PAL's exit service resumes the saved state, so
 * E_PalMain returns normally to the host. Saved: x19-x30, sp, x18 (the TEB), TPIDR_EL0 (the Linux
 * code changes it for its TLS), d8-d15.
 *
 * int64_t pal_vbs_ctx_save(struct pal_vbs_ctx* c);          returns 0, or v when resumed
 * void    pal_vbs_ctx_resume(struct pal_vbs_ctx* c, int64_t v);   does not return
 */
    .globl  pal_vbs_ctx_save
    .p2align 2
pal_vbs_ctx_save:
    stp     x19, x20, [x0, #0]
    stp     x21, x22, [x0, #16]
    stp     x23, x24, [x0, #32]
    stp     x25, x26, [x0, #48]
    stp     x27, x28, [x0, #64]
    stp     x29, x30, [x0, #80]
    mov     x1, sp
    str     x1, [x0, #96]
    str     x18, [x0, #104]
    mrs     x1, tpidr_el0
    str     x1, [x0, #112]
    stp     d8, d9, [x0, #120]
    stp     d10, d11, [x0, #136]
    stp     d12, d13, [x0, #152]
    stp     d14, d15, [x0, #168]
    mov     x0, #0
    ret

    .globl  pal_vbs_ctx_resume
    .p2align 2
pal_vbs_ctx_resume:
    ldp     x19, x20, [x0, #0]
    ldp     x21, x22, [x0, #16]
    ldp     x23, x24, [x0, #32]
    ldp     x25, x26, [x0, #48]
    ldp     x27, x28, [x0, #64]
    ldp     x29, x30, [x0, #80]
    ldr     x2, [x0, #96]
    mov     sp, x2
    ldr     x18, [x0, #104]
    ldr     x2, [x0, #112]
    msr     tpidr_el0, x2
    ldp     d8, d9, [x0, #120]
    ldp     d10, d11, [x0, #136]
    ldp     d12, d13, [x0, #152]
    ldp     d14, d15, [x0, #168]
    mov     x0, x1
    ret

/*
 * uint64_t pal_vbs_x18_spin(uint64_t iterations, uint64_t magic);
 *
 * Measures whether a value other than the TEB survives in x18 while the thread is preempted (an
 * application's glibc, not built with -ffixed-x18, may keep temporaries in x18). Puts `magic` in
 * x18, spins `iterations` times without calling anything, returns what x18 then holds, and
 * restores the TEB.
 */
    .globl  pal_vbs_x18_spin
    .p2align 2
pal_vbs_x18_spin:
    mov     x9, x18
    mov     x18, x1
1:  subs    x0, x0, #1
    b.ne    1b
    mov     x0, x18
    mov     x18, x9
    ret
