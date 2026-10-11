/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/*
 * This file contains the entry point of system call emulation in the LibOS
 * (`libos_syscall_entry`) and its non-returning counterpart `_return_from_syscall`, riscv64.
 *
 * Follows the aarch64 redesign (libos/src/arch/aarch64/syscallas.S) rather than the x86_64
 * original: riscv64's `ecall`, like aarch64's `svc`, is a plain instruction with *no* hardware
 * register-clobber contract that the emulation could piggyback on (x86_64's `syscall` itself
 * destroys RCX/R11 and leaves the resume address in RCX, which is what the x86_64 entry
 * convention is built on). So the resume address travels in an explicitly-passed register, and
 * the application's own live link register travels untouched: at a riscv64 `ecall` site `ra`
 * (x1) is the application's live return address -- the aarch64 port's finding #1
 * (Documentation/devel/aarch64/direct-pal-port-log.md) applies verbatim, including its symptom:
 * clobbering it turns the syscall wrapper's `ret` into an infinite self-loop.
 *
 * Entry contract:
 *   t0 (x5)  = resume address (where the application continues after this syscall). Carried --
 *              and *mirrored into regs[5]* -- exactly like aarch64's X16: syscall restart
 *              (restart_syscall() in libos_context.c) overwrites only context->pc, and
 *              _return_from_syscall() restoring t0 from regs[5] re-establishes the contract for
 *              the re-entry.
 *   a7       = syscall number (riscv64's own syscall ABI register, so the seccomp-trap path
 *              needs no shuffling at all: maybe_emulate_syscall() leaves it where the kernel put
 *              it and only writes regs[5]/context->pc).
 *   a0-a5    = syscall args (a6 is the rare 7th argument, saved/restored as ordinary state too).
 *   sp       = application's current stack pointer.
 *   all other GPRs hold live application state and are preserved.
 *
 * The sanctioned clobber set is `t0` (repurposed as the resume mirror) and `t1` (x6) -- the same
 * two registers gramine_entry_api.h's riscv64 GRAMINE_SYSCALL macro already clobbers
 * (`auipc/addi t0` + the GOT load's `la t1`), and both are caller-saved under the psABI, so no
 * well-behaved application holds live state in them across a call-like transition. Note this is
 * a *narrower* set than aarch64's X16/X17 in one respect and *broader* in another: glibc's
 * riscv64 syscall stubs declare only "memory" as clobbered (their __SYSCALL_CLOBBERS, unlike
 * x86_64's, lists no scratch registers), so this entry code goes out of its way to preserve
 * t2-t6/a6/a7 by stashing them on the application's own stack across the TCB lookup below --
 * only the two registers above may end up with emulated values. `t1` in particular is *never*
 * reloaded from the context by _return_from_syscall() (it is the branch-target register there),
 * which is why the spill below leaves regs[6] stale on purpose, mirroring aarch64's regs[17].
 */

#ifdef PAL_HOST_VM
#error "the riscv64 LibOS supports the Linux PAL only (Documentation/devel/riscv/plan.md scope)"
#endif

#include "asm_offsets.h"

.extern libos_emulate_syscall
.extern libos_xstate_size
.extern libos_xstate_restore
.extern g_pal_tcb_singleton
.extern g_pal_tcb_registry

/* PAL_PSTATE_EXACT_CONTEXT, spelled out for assembly (pal_arch.h's definition is a C size_t
 * expression). Checked by a static_assert in libos_context.c. */
#define EXACT_CONTEXT_TAG 0x4000000000000000

.global libos_syscall_entry
.type libos_syscall_entry, @function
.balign 4
libos_syscall_entry:
    .cfi_startproc

    /* Resolve *this thread's own* PAL_TCB, mirroring the aarch64 scan: `g_pal_tcb_registry` maps
     * a thread's application TLS pointer -- installed into `tp` by the clone()/fork() child's
     * very first act, hence always present before that thread's first syscall -- to its PAL_TCB.
     * Entries are {key = tls|1, tcb}, 0 = empty; deregistration at thread exit frees slots at
     * arbitrary indices, so the scan must cover the *whole* table and must not stop at the first
     * empty slot (stopping early would resolve the singleton and run this thread's syscalls on
     * the *main* thread's LibOS stack). `tp` bit 0 set = a Gramine-internal thread whose TLS
     * register pal_set_tcb() still owns: the tagged pointer decodes to its own TCB directly.
     *
     * Unlike aarch64 -- where the scan fits in X17 (one scratch) with X16/X8 pushed as a pair --
     * the scan below needs four live values at once (key, cursor, matched TCB, end mask) and
     * riscv64's contract permits only `t1`. So the other three scratch registers (t2, t3, t4)
     * plus the two values that must not be destroyed (t0 = resume address, a7 = syscall number)
     * are stashed on the application's stack first and reloaded verbatim at .Ltcb_done. 48 bytes
     * keeps SP 16-byte aligned as required at any call site, including for the 256-byte red zone
     * (there is none on riscv64) and for any signal taken in the middle of the scan.
     *
     * A genuine registry miss can only be the main thread before its first pal_get_tcb()
     * self-registration, or the startup window before the table is populated; both are exactly
     * what the g_pal_tcb_singleton fallback is correct for. */
    addi  sp, sp, -48
    sd    t2, 0(sp)
    sd    t3, 8(sp)
    sd    t4, 16(sp)
    sd    t0, 24(sp)                  /* resume address */
    sd    a7, 32(sp)                  /* syscall number */

    mv    t1, tp                      /* riscv64's TLS register is the GPR `tp` (x4), readable */
    andi  t2, t1, 1
    bnez  t2, .Ltcb_tagged
    ori   t1, t1, 1                   /* lookup key (slot keys are stored as `tls|1`) */

    /* The registry lives in libpal.so, i.e. cross-object from this shared library (libsysdb.so),
     * so its address must come through the GOT; `la` emits exactly that (auipc + ld through the
     * GOT slot). Same for the singleton fallback below. */
    la    t3, g_pal_tcb_registry
.Ltcb_scan:
    ld    t4, 0(t3)                   /* entry key (0 = empty slot) */
    bne   t4, t1, .Ltcb_next
    ld    t4, 8(t3)                   /* key matched: t4 = entry's PAL_TCB (cursor kept intact,
                                       * it is still needed to walk past a transient hole) */
    bnez  t4, .Ltcb_found
.Ltcb_next:
    addi  t3, t3, 16
    /* End of table: the exact bit test aarch64 does with `tst x17, #2047`.
     * g_pal_tcb_registry is declared __attribute__((aligned(LIBOS_TCB_REGISTRY_MAX * 16))) --
     * 128 entries * 16 bytes = 2048, asserted by a static_assert in pal_arch_exception.c -- so
     * the cursor realigns to a multiple of 2048 precisely one past the last entry. */
    li    t4, 2047
    and   t4, t3, t4
    bnez  t4, .Ltcb_scan
.Ltcb_miss:
    la    t3, g_pal_tcb_singleton
    ld    t1, 0(t3)                   /* t1 = *g_pal_tcb_singleton */
    j     .Ltcb_done
.Ltcb_found:
    mv    t1, t4                      /* t1 = resolved PAL_TCB */
    j     .Ltcb_done
.Ltcb_tagged:
    andi  t1, t1, -2                  /* strip pal_set_tcb()'s tag: t1 = PAL_TCB */
.Ltcb_done:
    /* Everything the scan borrowed is given back, so from here the *only* register whose value
     * has an emulated meaning is t0 (it always carried the resume address). See the file
     * comment's clobber-set note. */
    ld    t2, 0(sp)
    ld    t3, 8(sp)
    ld    t4, 16(sp)
    ld    t0, 24(sp)
    ld    a7, 32(sp)
    addi  sp, sp, 48

.Ltcb_ready:
    /* Find this thread's LibOS-internal stack top in the resolved PAL_TCB, and put the
     * PAL_CONTEXT immediately below it. */
    li    t2, (LIBOS_TCB_OFF + LIBOS_TCB_LIBOS_STACK_OFF)
    add   t1, t1, t2
    ld    t1, 0(t1)                   /* libos_tcb.libos_stack (top) */
    andi  t1, t1, -16                 /* defensive 16-byte realign */
    li    t2, ((PAL_CONTEXT_SIZE + 15) & ~15)
    sub   t1, t1, t2                  /* t1 = PAL_CONTEXT base (0x140, already a multiple of 16,
                                       * so SP stays 16-byte aligned when it becomes SP below) */

    /* The two values only the application's *view* of the machine holds: where to resume (t0,
     * written both as context->pc and as regs[5]'s permanent resume mirror -- the latter is what
     * syscall restart relies on, see restart_syscall()), and the application's stack pointer
     * (which is regs[2] outright on riscv64: PAL_CONTEXT.regs[] is indexed by the real ABI
     * register numbers, so there is no separate SP field to keep in sync). */
    sd    t0, PAL_CONTEXT_PC_OFF(t1)
    sd    t0, (PAL_CONTEXT_REGS_OFF + 8*5)(t1)
    sd    sp, (PAL_CONTEXT_REGS_OFF + 8*2)(t1)
    /* a0's first-argument snapshot, for restart_syscall() to put back after do_syscall() wrote
     * an -ERESTART* code over it (the riscv64 instance of pt_regs->orig_a0; a0 doubles as both
     * arg0 and the return-value register). */
    sd    a0, PAL_CONTEXT_ORIG_A0_OFF(t1)

    mv    sp, t1                      /* switch to the LibOS stack; t1 = PAL_CONTEXT base */

    /* Save the GPRs into context->regs[]. Indexed by real register number: ra=x1, sp=x2 (stored
     * above), tp=x4 (the application's TLS pointer -- live application state, saved like any
     * other), a0-a7=x10-x17, and so on. regs[5] (t0) was written above with the resume address
     * and regs[6] (t1) is *deliberately left stale*: it holds this context pointer / a TCB value,
     * and _return_from_syscall() never restores it (it is the branch-target register there).
     * riscv64 has no register pair stores, so this is one `sd` per register, with `t1` -- the
     * address register -- restored from itself last (a store's address and data registers are
     * both read before the write, so this single instruction reads the old base and installs the
     * stale scratch value into regs[6]). */
    sd    x1,  (PAL_CONTEXT_REGS_OFF + 8*1)(t1)   /* ra  */
    sd    x3,  (PAL_CONTEXT_REGS_OFF + 8*3)(t1)   /* gp  */
    sd    x4,  (PAL_CONTEXT_REGS_OFF + 8*4)(t1)   /* tp  */
    sd    x7,  (PAL_CONTEXT_REGS_OFF + 8*7)(t1)   /* t2  */
    sd    x8,  (PAL_CONTEXT_REGS_OFF + 8*8)(t1)   /* s0  */
    sd    x9,  (PAL_CONTEXT_REGS_OFF + 8*9)(t1)   /* s1  */
    sd    x10, (PAL_CONTEXT_REGS_OFF + 8*10)(t1)  /* a0  */
    sd    x11, (PAL_CONTEXT_REGS_OFF + 8*11)(t1)  /* a1  */
    sd    x12, (PAL_CONTEXT_REGS_OFF + 8*12)(t1)  /* a2  */
    sd    x13, (PAL_CONTEXT_REGS_OFF + 8*13)(t1)  /* a3  */
    sd    x14, (PAL_CONTEXT_REGS_OFF + 8*14)(t1)  /* a4  */
    sd    x15, (PAL_CONTEXT_REGS_OFF + 8*15)(t1)  /* a5  */
    sd    x16, (PAL_CONTEXT_REGS_OFF + 8*16)(t1)  /* a6  */
    sd    x17, (PAL_CONTEXT_REGS_OFF + 8*17)(t1)  /* a7: the syscall number, kept as live state
                                                   * (riscv64 keeps it separate from the args, so
                                                   * nothing has to reconstruct it on restart) */
    sd    x18, (PAL_CONTEXT_REGS_OFF + 8*18)(t1)  /* s2  */
    sd    x19, (PAL_CONTEXT_REGS_OFF + 8*19)(t1)  /* s3  */
    sd    x20, (PAL_CONTEXT_REGS_OFF + 8*20)(t1)  /* s4  */
    sd    x21, (PAL_CONTEXT_REGS_OFF + 8*21)(t1)  /* s5  */
    sd    x22, (PAL_CONTEXT_REGS_OFF + 8*22)(t1)  /* s6  */
    sd    x23, (PAL_CONTEXT_REGS_OFF + 8*23)(t1)  /* s7  */
    sd    x24, (PAL_CONTEXT_REGS_OFF + 8*24)(t1)  /* s8  */
    sd    x25, (PAL_CONTEXT_REGS_OFF + 8*25)(t1)  /* s9  */
    sd    x26, (PAL_CONTEXT_REGS_OFF + 8*26)(t1)  /* s10 */
    sd    x27, (PAL_CONTEXT_REGS_OFF + 8*27)(t1)  /* s11 */
    sd    x28, (PAL_CONTEXT_REGS_OFF + 8*28)(t1)  /* t3  */
    sd    x29, (PAL_CONTEXT_REGS_OFF + 8*29)(t1)  /* t4  */
    sd    x30, (PAL_CONTEXT_REGS_OFF + 8*30)(t1)  /* t5  */
    sd    x31, (PAL_CONTEXT_REGS_OFF + 8*31)(t1)  /* t6  */
    sd    t1,  (PAL_CONTEXT_REGS_OFF + 8*6)(t1)   /* t1: stale scratch, on purpose (above) */

    /* A syscall-entry context is not an exact host context: it resumes at the *resume address*
     * with only the ABI-visible state, which is the whole point of the fast path in
     * _return_from_syscall(). Hence no exact-restore tag (and no fault/ESR/oldmask content --
     * riscv64 has no ESR at all, the field is a port artifact kept at 0 for the shared code). */
    sd    zero, PAL_CONTEXT_PSTATE_OFF(t1)
    sd    zero, PAL_CONTEXT_FAULT_ADDRESS_OFF(t1)
    sd    zero, PAL_CONTEXT_ESR_OFF(t1)
    sd    zero, PAL_CONTEXT_OLDMASK_OFF(t1)

    /* FP control word only (single `fcsr` on riscv64 -- there is no separate FPSR/FPCR split),
     * and none of the F/D register file: matching x86_64/aarch64's choice of not capturing full
     * FP/SIMD state at syscall entry, since applications don't assume it survives a syscall.
     * Full state is captured only when a signal frame needs it (prepare_sigframe). Reading fcsr
     * requires the F extension's FS field to be enabled; the riscv64 Linux kernel never leaves
     * FS=Off for a user thread (it is Initial-or-Dirty from boot onward, with no lazy save/restore
     * of FP state in 6.6-era kernels), so this cannot trap on a syscall path. */
    csrr  t2, fcsr
    sw    t2, PAL_CONTEXT_FCSR_OFF(t1)
    sb    zero, PAL_CONTEXT_FPREGS_USED_OFF(t1)

    /* Reserve libos_xstate_size() bytes directly below the context for context->fpregs to point
     * at (unpopulated unless a signal needs full FP state while this syscall is in flight). The
     * context base is kept across the call in a slot of our *own* (LibOS) stack -- riscv64 has no
     * callee-saved register that is not also live application state here, and unlike aarch64
     * (which borrows X19 and restores it last) we need none: the value is memory, not a register
     * the application owns. */
    addi  sp, sp, -16
    sd    t1, 8(sp)
    call  libos_xstate_size           /* returns the size in a0 */
    ld    t1, 8(sp)
    sub   a0, t1, a0
    andi  a0, a0, -16                 /* LIBOS_XSTATE_ALIGN (16) */
    sd    a0, PAL_CONTEXT_FPREGS_OFF(t1)
    mv    sp, a0

    mv    a0, t1
    call  libos_emulate_syscall       /* does not return */

    /* Just to make the return address point inside this function, like x86_64's `ud2` and
     * aarch64's `brk #0`. */
    ebreak
    .cfi_endproc
.size libos_syscall_entry, .-libos_syscall_entry

.global _return_from_syscall
.type _return_from_syscall, @function
.balign 4
_return_from_syscall:
    /* Expects one argument (in a0): pointer to PAL_CONTEXT. Runs on the LibOS's own stack (the
     * C caller's frame), so the 16-byte frame below is Gramine's stack, not the application's. */
    .cfi_startproc
    addi  sp, sp, -16
    sd    a0, 0(sp)

    lbu   t1, PAL_CONTEXT_FPREGS_USED_OFF(a0)
    beqz  t1, .Lno_xstate

    ld    a0, PAL_CONTEXT_FPREGS_OFF(a0)
    call  libos_xstate_restore
    ld    a0, 0(sp)
    j     .Lexact_check

.Lno_xstate:
    lw    t1, PAL_CONTEXT_FCSR_OFF(a0)
    csrw  fcsr, t1

.Lexact_check:
    /* A context that came from the host -- an asynchronous signal or a fault interrupted the
     * application at an arbitrary instruction -- must be resumed *exactly*: the "restore the ABI
     * registers and jump" fast path below is only legitimate for syscall returns (it sacrifices
     * t1 and ignores e.g. the FP register file). Such contexts carry PAL_PSTATE_EXACT_CONTEXT in
     * pstate (set by ucontext_to_pal_context()); for them the PAL's SIGTRAP handler copies the
     * context into the host ucontext and lets the kernel's own sigreturn restore every register
     * -- including `ra`, which the fast path's branch register cannot. a0 = the PAL_CONTEXT,
     * a7 = the discriminator (riscv64's `ebreak` has no immediate field, unlike aarch64's
     * self-identifying `brk #imm`; see pal_arch_handle_trap()). */
    ld    t1, PAL_CONTEXT_PSTATE_OFF(a0)
    li    t2, EXACT_CONTEXT_TAG
    and   t2, t1, t2
    beqz  t2, .Lfast_restore
    li    a7, PAL_BRK_RESTORE_CONTEXT
    /* Pinned to the 4-byte encoding, by measurement: under the toolchain's default `-march`
     * (which includes C) gas assembles a bare `ebreak` to 2-byte `c.ebreak` as its *canonical*
     * encoding, and `.option norelax` does NOT prevent that -- verified in binutils 2.42 (the
     * relaxed-looking result is not a relaxation at all, so nothing that only disables
     * relaxations helps). `.option norvc` does: it makes the assembler emit the real 32-bit
     * instruction. pal_arch_handle_trap() recognizes both widths anyway, but pinning the one
     * instruction this protocol depends on keeps the discriminator semantics from depending on
     * what the toolchain happens to prefer. */
    .option push
    .option norvc
    ebreak
    .option pop
    j     .                          /* not reached: the trap handler resumes the application */

.Lfast_restore:
    /* From here we call nothing. The branch target lives in t1 (the sanctioned riscv64 scratch,
     * see the file comment) and is never reloaded from the context -- which is why the entry
     * spill leaves regs[6] stale. Unlike aarch64, no callee-saved register is borrowed as the
     * context base: a0 (x10) itself serves as the load base, and its application value (the
     * syscall return value the emulation stored there) is installed by the *final* load, which
     * reads the old base before writing it -- so all 32 registers except t1 come back from
     * memory, and the only register lost is the one the contract sacrifices anyway. */
    ld    t1, PAL_CONTEXT_PC_OFF(a0)                  /* branch target: resume pc, or a signal
                                                       * handler entry, or a restart re-entry */
    ld    x2, (PAL_CONTEXT_REGS_OFF + 8*2)(a0)         /* sp: back to the application's stack */
    ld    x1, (PAL_CONTEXT_REGS_OFF + 8*1)(a0)         /* ra: the application's live link register
                                                       * (aarch64 finding #1) */
    ld    x3, (PAL_CONTEXT_REGS_OFF + 8*3)(a0)         /* gp */
    ld    x4, (PAL_CONTEXT_REGS_OFF + 8*4)(a0)         /* tp: the TLS pointer -- restored as an
                                                       * ordinary register (riscv64's TLS register
                                                       * is a GPR; no msr needed, and nothing below
                                                       * uses TLS-relative addressing) */
    ld    x5, (PAL_CONTEXT_REGS_OFF + 8*5)(a0)         /* t0: the resume mirror (restart contract)
                                                       */
    /* regs[6] (t1) intentionally not restored -- the branch target occupies it, see above. */
    ld    x7,  (PAL_CONTEXT_REGS_OFF + 8*7)(a0)        /* t2 */
    ld    x8,  (PAL_CONTEXT_REGS_OFF + 8*8)(a0)        /* s0 */
    ld    x9,  (PAL_CONTEXT_REGS_OFF + 8*9)(a0)        /* s1 */
    ld    x11, (PAL_CONTEXT_REGS_OFF + 8*11)(a0)       /* a1 */
    ld    x12, (PAL_CONTEXT_REGS_OFF + 8*12)(a0)       /* a2 */
    ld    x13, (PAL_CONTEXT_REGS_OFF + 8*13)(a0)       /* a3 */
    ld    x14, (PAL_CONTEXT_REGS_OFF + 8*14)(a0)       /* a4 */
    ld    x15, (PAL_CONTEXT_REGS_OFF + 8*15)(a0)       /* a5 */
    ld    x16, (PAL_CONTEXT_REGS_OFF + 8*16)(a0)       /* a6 */
    ld    x17, (PAL_CONTEXT_REGS_OFF + 8*17)(a0)       /* a7 */
    ld    x18, (PAL_CONTEXT_REGS_OFF + 8*18)(a0)       /* s2 */
    ld    x19, (PAL_CONTEXT_REGS_OFF + 8*19)(a0)       /* s3 */
    ld    x20, (PAL_CONTEXT_REGS_OFF + 8*20)(a0)       /* s4 */
    ld    x21, (PAL_CONTEXT_REGS_OFF + 8*21)(a0)       /* s5 */
    ld    x22, (PAL_CONTEXT_REGS_OFF + 8*22)(a0)       /* s6 */
    ld    x23, (PAL_CONTEXT_REGS_OFF + 8*23)(a0)       /* s7 */
    ld    x24, (PAL_CONTEXT_REGS_OFF + 8*24)(a0)       /* s8 */
    ld    x25, (PAL_CONTEXT_REGS_OFF + 8*25)(a0)       /* s9 */
    ld    x26, (PAL_CONTEXT_REGS_OFF + 8*26)(a0)       /* s10 */
    ld    x27, (PAL_CONTEXT_REGS_OFF + 8*27)(a0)       /* s11 */
    ld    x28, (PAL_CONTEXT_REGS_OFF + 8*28)(a0)       /* t3 */
    ld    x29, (PAL_CONTEXT_REGS_OFF + 8*29)(a0)       /* t4 */
    ld    x30, (PAL_CONTEXT_REGS_OFF + 8*30)(a0)       /* t5 */
    ld    x31, (PAL_CONTEXT_REGS_OFF + 8*31)(a0)       /* t6 */
    ld    a0, (PAL_CONTEXT_REGS_OFF + 8*10)(a0)        /* a0: the return value; last, reading the
                                                       * base it destroys */
    jr    t1
    .cfi_endproc
.size _return_from_syscall, .-_return_from_syscall

.section .note.GNU-stack,"",@progbits
