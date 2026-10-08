/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2020 Intel Corporation
 *                    Borys Popławski <borysp@invisiblethingslab.com>
 */

/*
 * This file contains the entry point of system call emulation in LibOS (`libos_syscall_entry`).
 *
 * Redesigned from scratch for aarch64, not a line-by-line port of the x86_64 version: that file
 * leans on x86_64 SYSCALL/SYSRET's own hardware register-clobber contract (RCX holds the resume
 * address, RCX/R11 are clobbered) to build a uniform entry convention. aarch64's `svc` has no
 * such contract at all (the kernel's exception entry/exit is invisible to userspace), so this
 * entry point is reached via a plain AAPCS64 call/return convention instead: callers (either
 * GRAMINE_SYSCALL's `blr`, or maybe_emulate_syscall()'s LR rewrite on the seccomp-trap path --
 * see libos_context.c) set LR (X30) to the resume address and branch here; we return to it via
 * X16 at the very end (see _return_from_syscall), not via `ret`.
 *
 * X16/X17 are AAPCS64's own designated intra-procedure-call scratch registers (IP0/IP1): by
 * convention no well-behaved caller relies on them surviving any call-like transition. We lean
 * on that throughout this file as our one pair of "free" scratch registers, the same role
 * x86_64's RCX/R11 play there (which the real `syscall` instruction itself clobbers).
 */

#include "asm_offsets.h"

.extern libos_emulate_syscall
.extern libos_xstate_size
.extern libos_xstate_restore
.extern g_pal_tcb_singleton

.global libos_syscall_entry
.type libos_syscall_entry, %function
.align 4
libos_syscall_entry:
    .cfi_startproc
    /* Entry contract: X0-X7 = syscall args (X0-X5 meaningful), X8 = syscall number, X30 (LR) =
     * resume address, SP = application's current stack pointer, every other GPR holds live
     * application state that must be preserved. */

    mov  x17, sp              /* X17 = original app SP, saved before we touch SP at all */

    /* Find the LibOS-internal stack top via g_pal_tcb_singleton (see pal_arch.h's long comment
     * on why this is a plain global rather than a register on this architecture). */
    adrp x16, g_pal_tcb_singleton
    ldr  x16, [x16, :lo12:g_pal_tcb_singleton]
    ldr  x16, [x16, #(LIBOS_TCB_OFF + LIBOS_TCB_LIBOS_STACK_OFF)]
    and  x16, x16, #0xfffffffffffffff0   /* defensive 16-byte realign */
    sub  x16, x16, #PAL_CONTEXT_SIZE
    mov  sp, x16                          /* switch to the LibOS stack; X16 = PAL_CONTEXT base */

    /* Save all GPRs except X16/X17 into context->regs[]. */
    stp  x0,  x1,  [x16, #(PAL_CONTEXT_REGS_OFF+0)]
    stp  x2,  x3,  [x16, #(PAL_CONTEXT_REGS_OFF+16)]
    stp  x4,  x5,  [x16, #(PAL_CONTEXT_REGS_OFF+32)]
    stp  x6,  x7,  [x16, #(PAL_CONTEXT_REGS_OFF+48)]
    stp  x8,  x9,  [x16, #(PAL_CONTEXT_REGS_OFF+64)]
    stp  x10, x11, [x16, #(PAL_CONTEXT_REGS_OFF+80)]
    stp  x12, x13, [x16, #(PAL_CONTEXT_REGS_OFF+96)]
    stp  x14, x15, [x16, #(PAL_CONTEXT_REGS_OFF+112)]
    /* regs[16]/regs[17] intentionally left stale: sacrificed scratch registers, see top comment. */
    stp  x18, x19, [x16, #(PAL_CONTEXT_REGS_OFF+144)]
    stp  x20, x21, [x16, #(PAL_CONTEXT_REGS_OFF+160)]
    stp  x22, x23, [x16, #(PAL_CONTEXT_REGS_OFF+176)]
    stp  x24, x25, [x16, #(PAL_CONTEXT_REGS_OFF+192)]
    stp  x26, x27, [x16, #(PAL_CONTEXT_REGS_OFF+208)]
    stp  x28, x29, [x16, #(PAL_CONTEXT_REGS_OFF+224)]
    str  x30,      [x16, #(PAL_CONTEXT_REGS_OFF+240)]

    str  x17, [x16, #PAL_CONTEXT_SP_OFF]       /* original app SP */
    str  x30, [x16, #PAL_CONTEXT_PC_OFF]       /* resume address */
    str  xzr, [x16, #PAL_CONTEXT_PSTATE_OFF]
    str  xzr, [x16, #PAL_CONTEXT_FAULT_ADDRESS_OFF]
    str  xzr, [x16, #PAL_CONTEXT_ESR_OFF]
    str  xzr, [x16, #PAL_CONTEXT_OLDMASK_OFF]

    /* Save FPSR/FPCR into the context's scalar fields (lightweight control state only; the full
     * FP/SIMD register file is *not* captured here, matching x86_64's approach of only saving
     * mxcsr/fpcw at syscall entry -- real applications don't assume full FP/SIMD state survives
     * a syscall, same reasoning the top-of-file x86_64 comment spells out). */
    mrs  x0, fpsr
    mrs  x1, fpcr
    str  w0, [x16, #PAL_CONTEXT_FPSR_OFF]
    str  w1, [x16, #PAL_CONTEXT_FPCR_OFF]
    strb wzr, [x16, #PAL_CONTEXT_FPREGS_USED_OFF]

    mov  x19, x16   /* X19 (callee-saved) = PAL_CONTEXT base, kept across the calls below */

    /* Reserve libos_xstate_size() bytes directly below the context for context->fpregs to point
     * at (unpopulated for now -- only filled in if a signal needs full FP/SIMD state while this
     * syscall is being handled). */
    bl   libos_xstate_size
    sub  x16, x19, x0
    and  x16, x16, #0xfffffffffffffff0
    mov  sp, x16
    str  x16, [x19, #PAL_CONTEXT_FPREGS_OFF]

    mov  x0, x19
    bl   libos_emulate_syscall    /* does not return */

    /* Just to make the return address point inside this function, same as x86_64's `ud2`. */
    brk  #0
    .cfi_endproc
.size libos_syscall_entry, .-libos_syscall_entry

.global _return_from_syscall
.type _return_from_syscall, %function
.align 4
_return_from_syscall:
    /* expects one argument (in X0) -- pointer to PAL_CONTEXT */
    .cfi_startproc
    mov  x19, x0   /* X19 (callee-saved) holds the context pointer across the possible `bl` below */

    ldrb w0, [x19, #PAL_CONTEXT_FPREGS_USED_OFF]
    cbz  w0, .Lno_xstate

    ldr  x0, [x19, #PAL_CONTEXT_FPREGS_OFF]
    bl   libos_xstate_restore
    b    .Lgpr_restore

.Lno_xstate:
    ldr  w0, [x19, #PAL_CONTEXT_FPSR_OFF]
    ldr  w1, [x19, #PAL_CONTEXT_FPCR_OFF]
    msr  fpsr, x0
    msr  fpcr, x1

.Lgpr_restore:
    /* After this point we call nothing else, so X16/X17 are fair game as pure scratch for the
     * rest of this function (see the top-of-file comment on why that's safe). */
    ldp  x0,  x1,  [x19, #(PAL_CONTEXT_REGS_OFF+0)]
    ldp  x2,  x3,  [x19, #(PAL_CONTEXT_REGS_OFF+16)]
    ldp  x4,  x5,  [x19, #(PAL_CONTEXT_REGS_OFF+32)]
    ldp  x6,  x7,  [x19, #(PAL_CONTEXT_REGS_OFF+48)]
    ldp  x8,  x9,  [x19, #(PAL_CONTEXT_REGS_OFF+64)]
    ldp  x10, x11, [x19, #(PAL_CONTEXT_REGS_OFF+80)]
    ldp  x12, x13, [x19, #(PAL_CONTEXT_REGS_OFF+96)]
    ldp  x14, x15, [x19, #(PAL_CONTEXT_REGS_OFF+112)]
    /* regs[16]/regs[17] intentionally not restored -- sacrificed scratch registers. */
    ldp  x20, x21, [x19, #(PAL_CONTEXT_REGS_OFF+160)]
    ldp  x22, x23, [x19, #(PAL_CONTEXT_REGS_OFF+176)]
    ldp  x24, x25, [x19, #(PAL_CONTEXT_REGS_OFF+192)]
    ldp  x26, x27, [x19, #(PAL_CONTEXT_REGS_OFF+208)]
    ldp  x28, x29, [x19, #(PAL_CONTEXT_REGS_OFF+224)]
    ldr  x30,      [x19, #(PAL_CONTEXT_REGS_OFF+240)]

    ldr  x16, [x19, #PAL_CONTEXT_SP_OFF]
    mov  sp, x16
    ldr  x16, [x19, #PAL_CONTEXT_PC_OFF]   /* X16 = jump target; reused as scratch, fine */

    /* Restore X18 and (last of all) X19 itself -- we've been using X19 as our context-pointer
     * base this whole time, so its own true value has to come out via a temporary (X17, also
     * sacrificed scratch) rather than being overwritten mid-use. */
    ldp  x18, x17, [x19, #(PAL_CONTEXT_REGS_OFF+144)]
    mov  x19, x17

    br   x16
    .cfi_endproc
.size _return_from_syscall, .-_return_from_syscall

.section .note.GNU-stack,"",%progbits
