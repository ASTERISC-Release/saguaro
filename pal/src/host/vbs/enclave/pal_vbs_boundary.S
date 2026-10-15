/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

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

/*
 * uint64_t pal_vbs_spin_on_stack(uint64_t iterations, void* stack_top);
 * Switches SP to `stack_top` (memory outside the thread's Windows stack, as the LibOS's and the
 * application's stacks are), spins, switches back. Measures whether a thread preempted while its
 * SP is outside the TEB's stack range survives.
 */
    .globl  pal_vbs_spin_on_stack
    .p2align 2
pal_vbs_spin_on_stack:
    mov     x9, sp
    and     x1, x1, #~15
    mov     sp, x1
    stp     x9, x30, [sp, #-16]!
1:  subs    x0, x0, #1
    b.ne    1b
    ldp     x9, x30, [sp], #16
    mov     sp, x9
    mov     x0, #0
    ret

/*
 * int64_t pal_vbs_brk_on_stack(void* stack_top);
 * Raises a breakpoint with SP outside the Windows stack (caught by SEH and resumed past, if the
 * platform can dispatch it there), then switches back.
 */
    .globl  pal_vbs_brk_on_stack
    .p2align 2
pal_vbs_brk_on_stack:
    mov     x9, sp
    and     x0, x0, #~15
    mov     sp, x0
    stp     x9, x30, [sp, #-16]!
    brk     #0
    ldp     x9, x30, [sp], #16
    mov     sp, x9
    mov     x0, #0
    ret

/*
 * int64_t pal_vbs_brk_probe(void* stack_top);
 * Like pal_vbs_brk_on_stack, but the function has SEH unwind data naming a language-specific
 * exception handler (pal_vbs_probe_handler), as packed code could get from static .pdata/.xdata:
 * the dispatcher calls the handler for the faulting frame instead of unwinding further.
 */
    .globl  pal_vbs_brk_probe
    .p2align 2
    .seh_proc pal_vbs_brk_probe
    .seh_handler pal_vbs_probe_handler, @except
pal_vbs_brk_probe:
    .seh_endprologue
    mov     x9, sp
    and     x0, x0, #~15
    mov     sp, x0
    stp     x9, x30, [sp, #-16]!
    brk     #0
    ldp     x9, x30, [sp], #16
    mov     sp, x9
    mov     x0, #0
    ret
    .seh_endproc

/*
 * uint64_t pal_vbs_regs_spin(uint64_t iterations, uint64_t* out_tpidr);
 * Puts known patterns in TPIDR_EL0, x0-x8, x10-x17, x19-x28 and v0-v7, spins `iterations` times
 * without calling anything, then returns a bit mask of the registers that changed: bit i = xi
 * (i != 9, 18), bit 32+j = vj, bit 63 = TPIDR_EL0. The value TPIDR_EL0 held afterwards is stored in
 * *out_tpidr; TPIDR_EL0 and the callee-saved registers are restored.
 */
    .globl  pal_vbs_regs_spin
    .p2align 2
pal_vbs_regs_spin:
    stp     x29, x30, [sp, #-128]!
    mov     x29, sp
    stp     x19, x20, [sp, #16]
    stp     x21, x22, [sp, #32]
    stp     x23, x24, [sp, #48]
    stp     x25, x26, [sp, #64]
    stp     x27, x28, [sp, #80]
    stp     d8, d9, [sp, #96]
    mrs     x9, tpidr_el0
    stp     x9, x1, [sp, #112]          /* saved TPIDR_EL0, out pointer */
    mov     x9, x0                      /* loop counter */
    movz    x0, #0x7ee1, lsl #48
    msr     tpidr_el0, x0               /* TPIDR_EL0 = 0x7ee1000000000000 */
    movz    x0, #0xa000
    movz    x1, #0xa001
    movz    x2, #0xa002
    movz    x3, #0xa003
    movz    x4, #0xa004
    movz    x5, #0xa005
    movz    x6, #0xa006
    movz    x7, #0xa007
    movz    x8, #0xa008
    movz    x10, #0xa00a
    movz    x11, #0xa00b
    movz    x12, #0xa00c
    movz    x13, #0xa00d
    movz    x14, #0xa00e
    movz    x15, #0xa00f
    movz    x16, #0xa010
    movz    x17, #0xa011
    movz    x19, #0xa013
    movz    x20, #0xa014
    movz    x21, #0xa015
    movz    x22, #0xa016
    movz    x23, #0xa017
    movz    x24, #0xa018
    movz    x25, #0xa019
    movz    x26, #0xa01a
    movz    x27, #0xa01b
    movz    x28, #0xa01c
    dup     v0.2d, x0
    dup     v1.2d, x1
    dup     v2.2d, x2
    dup     v3.2d, x3
    dup     v4.2d, x4
    dup     v5.2d, x5
    dup     v6.2d, x6
    dup     v7.2d, x7
1:  subs    x9, x9, #1
    b.ne    1b
    /* x9 = result mask; check each register (x29/x30 are on the stack frame) */
    mov     x9, #0
    .macro chkx reg, val, bit
    movz    x29, #\val
    cmp     \reg, x29
    b.eq    2f
    orr     x9, x9, #(1 << \bit)
2:
    .endm
    chkx    x0, 0xa000, 0
    chkx    x1, 0xa001, 1
    chkx    x2, 0xa002, 2
    chkx    x3, 0xa003, 3
    chkx    x4, 0xa004, 4
    chkx    x5, 0xa005, 5
    chkx    x6, 0xa006, 6
    chkx    x7, 0xa007, 7
    chkx    x8, 0xa008, 8
    chkx    x10, 0xa00a, 10
    chkx    x11, 0xa00b, 11
    chkx    x12, 0xa00c, 12
    chkx    x13, 0xa00d, 13
    chkx    x14, 0xa00e, 14
    chkx    x15, 0xa00f, 15
    chkx    x16, 0xa010, 16
    chkx    x17, 0xa011, 17
    chkx    x19, 0xa013, 19
    chkx    x20, 0xa014, 20
    chkx    x21, 0xa015, 21
    chkx    x22, 0xa016, 22
    chkx    x23, 0xa017, 23
    chkx    x24, 0xa018, 24
    chkx    x25, 0xa019, 25
    chkx    x26, 0xa01a, 26
    chkx    x27, 0xa01b, 27
    chkx    x28, 0xa01c, 28
    .macro chkv vreg, val, bit
    fmov    x29, d\vreg
    movz    x0, #\val
    cmp     x29, x0
    b.eq    3f
    orr     x9, x9, #(1 << \bit)
3:
    .endm
    chkv    0, 0xa000, 32
    chkv    1, 0xa001, 33
    chkv    2, 0xa002, 34
    chkv    3, 0xa003, 35
    chkv    4, 0xa004, 36
    chkv    5, 0xa005, 37
    chkv    6, 0xa006, 38
    chkv    7, 0xa007, 39
    mrs     x0, tpidr_el0
    ldp     x1, x2, [sp, #112]          /* x1 = saved TPIDR_EL0, x2 = out pointer */
    str     x0, [x2]
    movz    x29, #0x7ee1, lsl #48
    cmp     x0, x29
    b.eq    4f
    orr     x9, x9, #(1 << 63)
4:  msr     tpidr_el0, x1
    mov     x0, x9
    ldp     d8, d9, [sp, #96]
    ldp     x27, x28, [sp, #80]
    ldp     x25, x26, [sp, #64]
    ldp     x23, x24, [sp, #48]
    ldp     x21, x22, [sp, #32]
    ldp     x19, x20, [sp, #16]
    ldp     x29, x30, [sp], #128
    ret

/* uint64_t pal_vbs_get_tpidr(void); void pal_vbs_set_tpidr(uint64_t v); */
    .globl  pal_vbs_get_tpidr
    .p2align 2
pal_vbs_get_tpidr:
    mrs     x0, tpidr_el0
    ret
    .globl  pal_vbs_set_tpidr
    .p2align 2
pal_vbs_set_tpidr:
    msr     tpidr_el0, x0
    ret

/*
 * uint64_t pal_vbs_tpidr_spin(uint64_t iterations, uint64_t magic, uint64_t* out_first);
 * Sets TPIDR_EL0 = magic and checks it on every iteration of a tight loop; on a mismatch counts it,
 * records the first value seen and sets it again. Returns the number of mismatches; restores the
 * original TPIDR_EL0.
 */
    .globl  pal_vbs_tpidr_spin
    .p2align 2
pal_vbs_tpidr_spin:
    mrs     x9, tpidr_el0
    mov     x10, #0                 /* mismatches */
    msr     tpidr_el0, x1
1:  mrs     x11, tpidr_el0
    cmp     x11, x1
    b.eq    2f
    cbnz    x10, 3f
    str     x11, [x2]
3:  add     x10, x10, #1
    msr     tpidr_el0, x1
2:  subs    x0, x0, #1
    b.ne    1b
    msr     tpidr_el0, x9
    mov     x0, x10
    ret
