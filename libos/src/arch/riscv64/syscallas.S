/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/*
 * riscv64 syscall entry/return assembly (riscv64 port of the aarch64 syscallas.S).
 *
 * PHASE-1 STUB (Documentation/devel/riscv/plan.md, Phase 1: "the whole tree compiles and links
 * for riscv64 with stubs that abort"): this file defines the two link-visible entry points the
 * generic LibOS code references --
 *
 *   void* libos_syscall_entry        (stored into every TCB by libos_tcb.h's
 *                                     libos_tcb_init(), referenced from gramine_entry_api.h's
 *                                     GRAMINE_SYSCALL chain and the vdso)
 *   void  _return_from_syscall(PAL_CONTEXT*)   (noreturn; called by the C upcall machinery to
 *                                     finish a syscall)
 *
 * -- as immediate runtime aborts. The real implementation (Phase 4 of the plan) will be a
 * riscv64 translation of the aarch64 file: the per-thread PAL_TCB registry scan via `la` GOT
 * load, the resume-address contract in t0 (see gramine_entry_api.h's fixed-size
 * GRAMINE_SYSCALL macro), the orig_a0 snapshot (a0 doubles as arg0 and return value, like x0 on
 * aarch64 -- finding #6 / pt_regs->orig_a0), the exact-context-restore `ebreak` with the
 * PAL_BRK_RESTORE_CONTEXT a7 discriminator (finding #15's riscv64 counterpart: riscv64 has no
 * condition flags so only the register half of that finding applies, but ra is live at an
 * arbitrary interrupt point and no `jr` can restore it while itself surviving), and the
 * clone-child return path. See Documentation/devel/aarch64/direct-pal-port-log.md findings #1,
 * #3, #6, #13, #15.
 */

#define __ASSEMBLY__

.text

.global libos_syscall_entry
.type libos_syscall_entry, %function
.align 4
libos_syscall_entry:
    .cfi_startproc
    /* Phase-1 abort stub. */
.Labort_syscall_entry:
    ebreak
    j .Labort_syscall_entry
    .cfi_endproc

.global _return_from_syscall
.type _return_from_syscall, %function
.align 4
_return_from_syscall:
    .cfi_startproc
    /* Phase-1 abort stub. */
.Labort_return_from_syscall:
    ebreak
    j .Labort_return_from_syscall
    .cfi_endproc

.section .note.GNU-stack,"",%progbits
