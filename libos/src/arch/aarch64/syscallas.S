/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */
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
 * entry point is reached via explicit register passing instead: callers (either GRAMINE_SYSCALL's
 * `adr x16, 1f; br`, or maybe_emulate_syscall() planting it in the ucontext's regs[16] on the
 * seccomp-trap path -- see libos_context.c) place the resume address in X16 and branch here.
 *
 * Entry contract:
 *   X16 = resume address (where the application should continue after this syscall)
 *   X0-X7 = syscall args (X0-X5 meaningful), X8 = syscall number
 *   SP = application's current stack pointer
 *   every other GPR (X9-X15, X17-X30) holds live application state that must be preserved.
 *
 * X30 (LR) is deliberately NOT the resume-address carrier, mirroring neither x86_64's RCX nor any
 * other choice involving it: unlike x86_64's RCX -- which the hardware `syscall` instruction
 * itself destroys, so no application can ever hold live state in it at a syscall site -- aarch64's
 * X30 at an `svc` site is the application's own live link register (leaf-ish libc syscall
 * wrappers `ret` via it right after the `svc`). Clobbering it silently corrupts the caller's
 * return path into an infinite loop (this was the root cause of the aarch64 helloworld spin; see
 * Documentation/devel/aarch64/direct-pal-port-log.md, finding #1). So the resume address travels
 * in X16 and the app's true X30 is saved and restored like any other live register.
 *
 * X16/X17 are AAPCS64's own designated intra-procedure-call scratch registers (IP0/IP1): by
 * convention no well-behaved caller relies on them surviving any call-like transition -- the
 * same role x86_64's RCX/R11 play there, which is why they are the sanctioned clobber set of
 * syscall emulation on aarch64. (X17 is used as pure scratch below and its application value is
 * not preserved; X16's saved slot in the context always mirrors the resume address, which is
 * what syscall-restart relies on -- see restart_syscall() in libos_context.c.)
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
    /* X16 = resume address (see entry contract above); it must be written into the context
     * before X16 is reused as scratch. X17 is the only register available to compute the
     * context address with (the GOT chain needs one), so the whole prologue is built around
     * X17, with X16 kept intact until it has been stashed. */

    /* Resolve *this thread's own* PAL_TCB. The table `g_pal_tcb_registry` (see its long comment
     * in pal_arch_exception.c) maps a thread's application TLS pointer -- installed into
     * TPIDR_EL0 by set_tls(), which is every clone()/fork() child's first act, hence always
     * present before a thread's first syscall -- to that thread's PAL_TCB. Entries are stored
     * {key = tls|1, tcb} with 0 = empty; deregistration frees slots at arbitrary indices, so
     * the scan below must cover the whole table. Tagged pointers (bit 0 set: Gramine-internal
     * threads whose TLS register pal_set_tcb() still owns) decode to their TCB directly.
     *
     * A genuine miss can only be the main thread before its first pal_get_tcb() self-registration
     * -- the one case for which the g_pal_tcb_singleton fallback is correct -- or the startup
     * window before the table is populated; both take the GOT fallback below. x16 (the resume
     * address, needed for the restart mirror) and x8 (syscall number, needed for dispatch) are
     * the only registers this scan may not destroy: both live in the application's stack frame
     * for the duration (the stack is still the application's own, 16-byte-aligned as the ABI
     * requires at any svc site), and are reloaded verbatim at .Ltcb_done. This replaces the
     * pre-multi-thread design that always read g_pal_tcb_singleton here -- correct for one
     * thread, catastrophic for more (all threads then emulated syscalls on *one shared* LibOS
     * stack). */
#ifdef HOST_TYPE
    /* VM-PAL (Milestone B): the TLS-keyed registry above is a Linux-PAL mechanism. In the
     * unikernel, libsysdb runs at EL0 where TPIDR_EL1 is unreadable and the kernel itself
     * resolves `svc` dispatch; it mirrors this thread's PAL_TCB into g_pal_tcb_singleton on
     * every context switch next to the cur_tcb store (pal_vm_tcb.c), so one GOT load IS the
     * "which thread am I" answer. x16 (resume address) and x8 (syscall nr) are never touched
     * here, so the prologue's stack stash below is not needed either. */
    adrp x17, :got:g_pal_tcb_singleton
    ldr  x17, [x17, :got_lo12:g_pal_tcb_singleton]
    ldr  x17, [x17]
    b    .Ltcb_tcb_ready
#else
    .extern g_pal_tcb_registry
    stp  x16, x8, [sp, #-16]!
    mrs  x16, tpidr_el0
    tbnz x16, #0, .Ltcb_tagged
    orr  x16, x16, #1                 /* lookup key (slot key is `tls|1`, 0 = empty marker) */
    /* The table itself lives in libpal.so (cross-object: see pal_arch_exception.c), so the base
     * must come through the GOT -- same pattern as the g_pal_tcb_singleton fallback below. */
    adrp x17, :got:g_pal_tcb_registry
    ldr  x17, [x17, :got_lo12:g_pal_tcb_registry]
    /* Full-table scan: x17 walks all 128 entries; the end is detected with `tst x17, #2047`
     * == 0, which is exact because g_pal_tcb_registry is declared
     * __attribute__((aligned(LIBOS_TCB_REGISTRY_MAX * 16))), so the cursor realigns to the
     * table size precisely one past the last entry. Scanning must NOT stop at the first empty
     * slot: thread-exit deregistration frees slots at arbitrary indices, so holes occur below
     * live entries; an early stop would truncate this thread's own higher-index entry and
     * resolve the singleton instead -- which would then run this entire syscall, including the
     * LIBOS_TCB context writes below, on the MAIN thread's TCB (wake-queue and context
     * corruption). A {key, tcb == NULL} pair is a mid-flight transient (add()'s two-step
     * publish or del()'s two-store removal): TLS keys are unique per live thread, so skipping
     * past it can never hide the scanning thread's own entry. */
.Ltcb_scan:
    ldr  x8, [x17]                   /* entry key (0 = empty slot) */
    cmp  x8, x16
    b.ne .Ltcb_next
    ldr  x8, [x17, #8]               /* key matched; x8 = entry's PAL_TCB (cursor kept intact --
                                      * it is still needed to walk past a transient hole) */
    cbnz x8, .Ltcb_found
    b    .Ltcb_next                  /* {key, NULL} transient (add()'s two-step publish or
                                      * del()'s two-store removal): skip it -- TLS keys are
                                      * unique per live thread, so this thread's own entry
                                      * cannot hide behind it, and following a dying thread's
                                      * about-to-be-freed TCB could fault. */
.Ltcb_found:
    mov  x17, x8                     /* x17 = resolved PAL_TCB */
    b    .Ltcb_done
.Ltcb_next:
    add  x17, x17, #16
    tst  x17, #2047
    b.ne .Ltcb_scan
.Ltcb_miss:
    adrp x17, :got:g_pal_tcb_singleton
    ldr  x17, [x17, :got_lo12:g_pal_tcb_singleton]
    ldr  x17, [x17]
    b    .Ltcb_done
.Ltcb_tagged:
    and  x17, x16, #-2                /* strip the pal_set_tcb() tag */
.Ltcb_done:
    ldp  x16, x8, [sp], #16
#endif

.Ltcb_tcb_ready:

    /* Find the LibOS-internal stack top in the resolved PAL_TCB. */
    ldr  x17, [x17, #(LIBOS_TCB_OFF + LIBOS_TCB_LIBOS_STACK_OFF)]
    and  x17, x17, #0xfffffffffffffff0   /* defensive 16-byte realign */
    sub  x17, x17, #PAL_CONTEXT_SIZE

    /* Stash the two values that only the application's view of the machine holds. X16 is spent
     * on the resume address first (its only carrier), then reused as the one scratch needed to
     * move SP into the context (A64 has no `str sp` form -- SP is not a general register). */
    str  x16, [x17, #PAL_CONTEXT_PC_OFF]  /* resume address */
    str  x16, [x17, #(PAL_CONTEXT_REGS_OFF+128)] /* same value mirrored into regs[16] */
    mov  x16, sp
    str  x16, [x17, #PAL_CONTEXT_SP_OFF] /* original app SP */
    mov  sp, x17                          /* switch to the LibOS stack; X17 = PAL_CONTEXT base */

    /* Save all GPRs except X17 into context->regs[]. regs[16] was already written above with the
     * same resume value, so regs[16] *always* mirrors "where the syscall we are (re-)entering
     * returns to", which is what syscall restart (restart_syscall() jumping back to
     * libos_syscall_entry) relies on: _return_from_syscall() restores X16 from regs[16] before
     * branching, so a restart re-enters here with a still-correct resume address in X16. */
    stp  x0,  x1,  [x17, #(PAL_CONTEXT_REGS_OFF+0)]
    stp  x2,  x3,  [x17, #(PAL_CONTEXT_REGS_OFF+16)]
    stp  x4,  x5,  [x17, #(PAL_CONTEXT_REGS_OFF+32)]
    stp  x6,  x7,  [x17, #(PAL_CONTEXT_REGS_OFF+48)]
    stp  x8,  x9,  [x17, #(PAL_CONTEXT_REGS_OFF+64)]
    stp  x10, x11, [x17, #(PAL_CONTEXT_REGS_OFF+80)]
    stp  x12, x13, [x17, #(PAL_CONTEXT_REGS_OFF+96)]
    stp  x14, x15, [x17, #(PAL_CONTEXT_REGS_OFF+112)]
    /* regs[16] (the resume mirror) and regs[17] are handled in the prologue above / intentionally
     * left stale: sacrificed scratch registers, see top comment. */
    stp  x18, x19, [x17, #(PAL_CONTEXT_REGS_OFF+144)]
    stp  x20, x21, [x17, #(PAL_CONTEXT_REGS_OFF+160)]
    stp  x22, x23, [x17, #(PAL_CONTEXT_REGS_OFF+176)]
    stp  x24, x25, [x17, #(PAL_CONTEXT_REGS_OFF+192)]
    stp  x26, x27, [x17, #(PAL_CONTEXT_REGS_OFF+208)]
    stp  x28, x29, [x17, #(PAL_CONTEXT_REGS_OFF+224)]
    /* The application's live link register -- saved as plain application state, *not* conflated
     * with the resume address (that's context->pc, stored above from X16). See top comment. */
    str  x30,      [x17, #(PAL_CONTEXT_REGS_OFF+240)]

    str  xzr, [x17, #PAL_CONTEXT_PSTATE_OFF]
    str  xzr, [x17, #PAL_CONTEXT_FAULT_ADDRESS_OFF]
    str  xzr, [x17, #PAL_CONTEXT_ESR_OFF]
    str  xzr, [x17, #PAL_CONTEXT_OLDMASK_OFF]

    /* Save FPSR/FPCR into the context's scalar fields (lightweight control state only; the full
     * FP/SIMD register file is *not* captured here, matching x86_64's approach of only saving
     * mxcsr/fpcw at syscall entry -- real applications don't assume full FP/SIMD state survives
     * a syscall, same reasoning the top-of-file x86_64 comment spells out). */
    mrs  x0, fpsr
    mrs  x1, fpcr
    str  w0, [x17, #PAL_CONTEXT_FPSR_OFF]
    str  w1, [x17, #PAL_CONTEXT_FPCR_OFF]
    strb wzr, [x17, #PAL_CONTEXT_FPREGS_USED_OFF]

    mov  x19, x17   /* X19 (callee-saved) = PAL_CONTEXT base, kept across the calls below */

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
    /* A context that came from the host (an asynchronous signal or fault interrupted the
     * application at an arbitrary instruction) must be resumed *exactly*: the sacrificed-x17 jump
     * below is only legitimate for syscall returns. Such contexts are tagged with bit 62 of
     * pstate (PAL_PSTATE_EXACT_CONTEXT, pal_arch.h); for them the PAL's SIGTRAP handler restores
     * every register and the flags through the kernel's own sigreturn. x0 = the PAL_CONTEXT. */
    ldr  x0, [x19, #PAL_CONTEXT_PSTATE_OFF]
    tbz  x0, #62, .Lfast_restore
    mov  x0, x19
    brk  #PAL_BRK_RESTORE_CONTEXT
    brk  #0                      /* not reached: the trap handler resumes the application */
.Lfast_restore:
    /* After this point we call nothing else. X16 and X17 have specific final values from here
     * on -- X16 = the application's (per-contract clobbered, restart-meaningful) resume mirror,
     * X17 = the branch target (context->pc) -- so the remaining scratch work has to route
     * around them; X18 is used as the one spare, and the application's true X18 is reloaded
     * after SP is switched (loading into SP-adjacent state via the still-live X19 base is safe:
     * reading memory never disturbs registers). */
    ldp  x0,  x1,  [x19, #(PAL_CONTEXT_REGS_OFF+0)]
    ldp  x2,  x3,  [x19, #(PAL_CONTEXT_REGS_OFF+16)]
    ldp  x4,  x5,  [x19, #(PAL_CONTEXT_REGS_OFF+32)]
    ldp  x6,  x7,  [x19, #(PAL_CONTEXT_REGS_OFF+48)]
    ldp  x8,  x9,  [x19, #(PAL_CONTEXT_REGS_OFF+64)]
    ldp  x10, x11, [x19, #(PAL_CONTEXT_REGS_OFF+80)]
    ldp  x12, x13, [x19, #(PAL_CONTEXT_REGS_OFF+96)]
    ldp  x14, x15, [x19, #(PAL_CONTEXT_REGS_OFF+112)]
    ldr  x16, [x19, #(PAL_CONTEXT_REGS_OFF+128)] /* app X16 (== resume; see entry contract) */
    /* regs[17] intentionally not restored -- sacrificed scratch register. */
    ldp  x20, x21, [x19, #(PAL_CONTEXT_REGS_OFF+160)]
    ldp  x22, x23, [x19, #(PAL_CONTEXT_REGS_OFF+176)]
    ldp  x24, x25, [x19, #(PAL_CONTEXT_REGS_OFF+192)]
    ldp  x26, x27, [x19, #(PAL_CONTEXT_REGS_OFF+208)]
    ldp  x28, x29, [x19, #(PAL_CONTEXT_REGS_OFF+224)]
    ldr  x30,      [x19, #(PAL_CONTEXT_REGS_OFF+240)] /* app's live link register */

    ldr  x17, [x19, #PAL_CONTEXT_PC_OFF] /* branch target (resume pc or signal handler...) */
    ldr  x18, [x19, #PAL_CONTEXT_SP_OFF]
    mov  sp, x18                          /* switch back to the app stack */
    ldr  x18, [x19, #(PAL_CONTEXT_REGS_OFF+144)] /* app X18, reloaded via still-live X19 base */

    /* Restore X19 itself last -- we've been using it as our context-pointer base this whole
     * time; a load's address register is read before being written, so this single instruction
     * both reads the saved value through the old base and installs it. */
    ldr  x19, [x19, #(PAL_CONTEXT_REGS_OFF+152)]

    br   x17
    .cfi_endproc
.size _return_from_syscall, .-_return_from_syscall

.section .note.GNU-stack,"",%progbits
