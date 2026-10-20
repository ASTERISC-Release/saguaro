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
 * (dev docs: aarch64/direct/FINDINGS.md) applies verbatim, including its symptom:
 * clobbering it turns the syscall wrapper's `ret` into an infinite self-loop.
 *
 * Entry contract:
 *   libos_tcb.syscall_scratch_pc = resume address (where the application continues after this
 *              syscall), stored by whoever redirects the thread here: maybe_emulate_syscall() for
 *              a trapped `ecall` (libos_context.c). A restarted syscall arrives the same way:
 *              restart_syscall() steps the pc back onto the `ecall`.
 *   a7       = syscall number, a0-a5 = arguments (a6 is the rare 7th one).
 *   sp       = application's current stack pointer.
 *   every GPR holds live application state and is preserved.
 *
 * No register is sacrificed, neither here nor on the way back. glibc's riscv64 syscall stubs
 * declare only "memory" as clobbered (__SYSCALL_CLOBBERS), and the kernel's `ecall` indeed
 * changes nothing but a0, so the compiler keeps live values in t0-t6 across an inlined syscall.
 * An earlier version of this file used t0 to carry the resume address and t1 as the branch
 * register of the return path; a static binary then crashed right after its first ioctl(), with
 * a clobbered temporary. The entry therefore spills the four registers its TCB lookup needs onto
 * the application's stack (riscv64 has no red zone, so the space below `sp` is free), and
 * _return_from_syscall() has no register left to jump through, so it always asks the PAL for an
 * exact restore (see there).
 */

#ifdef PAL_HOST_VM
#error "the riscv64 LibOS supports the Linux PAL only (dev docs: riscv/PLAN.md scope)"
#endif

#include "asm_offsets.h"

.extern libos_emulate_syscall
.extern libos_xstate_size
.extern g_pal_tcb_singleton
.extern g_pal_tcb_registry
.extern libos_tcb_registry_scan
.extern pal_arch_restore_context

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
     * The scan needs four live values at once (key, cursor, matched TCB, end mask), so t1-t4
     * are stashed on the application's stack first and moved into the context afterwards. 48
     * bytes keeps SP 16-byte aligned, also for a signal taken in the middle of the scan.
     *
     * A genuine registry miss can only be the main thread before its first pal_get_tcb()
     * self-registration, or the startup window before the table is populated; both are exactly
     * what the g_pal_tcb_singleton fallback is correct for. */
    addi  sp, sp, -48
    sd    t1, 0(sp)
    sd    t2, 8(sp)
    sd    t3, 16(sp)
    sd    t4, 24(sp)

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
    /* End of table: g_pal_tcb_registry is declared aligned to its own size (128 entries * 16
     * bytes = 2048, asserted by a static_assert in pal_arch_exception.c), so the cursor realigns
     * to a multiple of 2048 precisely one past the last entry. */
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
    /* t2 = PAL_CONTEXT base: immediately below this thread's LibOS-internal stack top. Its size
     * is a multiple of 16, so SP stays 16-byte aligned when it becomes SP below. */
    li    t2, (LIBOS_TCB_OFF + LIBOS_TCB_LIBOS_STACK_OFF)
    add   t2, t1, t2
    ld    t2, 0(t2)                   /* libos_tcb.libos_stack (top) */
    andi  t2, t2, -16                 /* defensive 16-byte realign */
    li    t3, ((PAL_CONTEXT_SIZE + 15) & ~15)
    sub   t2, t2, t3

    /* context->pc = the resume address left for us in the TCB (see the entry contract). */
    li    t3, (LIBOS_TCB_OFF + LIBOS_TCB_SCRATCH_PC_OFF)
    add   t3, t1, t3
    ld    t3, 0(t3)
    sd    t3, PAL_CONTEXT_PC_OFF(t2)

    /* The four spilled registers go from the application's stack into the context, and the
     * application's stack pointer is put back. */
    ld    t3, 0(sp)
    sd    t3, (PAL_CONTEXT_REGS_OFF + 8*6)(t2)    /* t1 */
    ld    t3, 8(sp)
    sd    t3, (PAL_CONTEXT_REGS_OFF + 8*7)(t2)    /* t2 */
    ld    t3, 16(sp)
    sd    t3, (PAL_CONTEXT_REGS_OFF + 8*28)(t2)   /* t3 */
    ld    t3, 24(sp)
    sd    t3, (PAL_CONTEXT_REGS_OFF + 8*29)(t2)   /* t4 */
    addi  sp, sp, 48

    mv    t1, t2                      /* t1 = PAL_CONTEXT base from here on */
    sd    sp, (PAL_CONTEXT_REGS_OFF + 8*2)(t1)
    /* a0's first-argument snapshot, for restart_syscall() to put back after do_syscall() wrote
     * an -ERESTART* code over it (the riscv64 instance of pt_regs->orig_a0; a0 doubles as both
     * arg0 and the return-value register). */
    sd    a0, PAL_CONTEXT_ORIG_A0_OFF(t1)

    mv    sp, t1                      /* switch to the LibOS stack */

    /* Save the remaining GPRs into context->regs[], indexed by real register number (sp and
     * t1-t4 were stored above). */
    sd    x1,  (PAL_CONTEXT_REGS_OFF + 8*1)(t1)   /* ra  */
    sd    x3,  (PAL_CONTEXT_REGS_OFF + 8*3)(t1)   /* gp  */
    sd    x4,  (PAL_CONTEXT_REGS_OFF + 8*4)(t1)   /* tp  */
    sd    x5,  (PAL_CONTEXT_REGS_OFF + 8*5)(t1)   /* t0  */
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
    sd    x30, (PAL_CONTEXT_REGS_OFF + 8*30)(t1)  /* t5  */
    sd    x31, (PAL_CONTEXT_REGS_OFF + 8*31)(t1)  /* t6  */

    /* No fault/ESR/oldmask content (riscv64 has no ESR at all, the field is a port artifact kept
     * at 0 for the shared code). */
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

/* Entry from a rewritten application `ecall`. The per-site stub has saved t0..t4 on the
 * application stack and passes the resume PC in t1. Use the PAL's C registry accessor rather
 * than duplicate its acquire-load protocol here. The call needs a normal ABI frame, so save every
 * caller-saved application register first; the normal syscall entry saves the restored registers
 * into the eventual PAL_CONTEXT. */
.global libos_syscall_entry_patched
.type libos_syscall_entry_patched, @function
.balign 4
libos_syscall_entry_patched:
    addi  sp, sp, -144
    sd    ra,  0(sp)
    sd    t0,  8(sp)
    sd    t1, 16(sp)                 /* resume address from the stub */
    sd    t2, 24(sp)
    sd    t3, 32(sp)
    sd    t4, 40(sp)
    sd    t5, 48(sp)
    sd    t6, 56(sp)
    sd    a0, 64(sp)
    sd    a1, 72(sp)
    sd    a2, 80(sp)
    sd    a3, 88(sp)
    sd    a4, 96(sp)
    sd    a5,104(sp)
    sd    a6,112(sp)
    sd    a7,120(sp)
    addi  a0, sp, 128
    call  libos_tcb_registry_scan@plt
    bnez  a0, .Lpatched_found
    la    t0, g_pal_tcb_singleton
    ld    t0, 0(t0)
    j     .Lpatched_store
.Lpatched_found:
    ld    t0, 128(sp)
.Lpatched_store:
    li    t1, (LIBOS_TCB_OFF + LIBOS_TCB_SCRATCH_PC_OFF)
    add   t1, t0, t1
    ld    t0, 16(sp)
    sd    t0, 0(t1)
    ld    ra,  0(sp)
    ld    t0,  8(sp)
    ld    t1, 16(sp)
    ld    t2, 24(sp)
    ld    t3, 32(sp)
    ld    t4, 40(sp)
    ld    t5, 48(sp)
    ld    t6, 56(sp)
    ld    a0, 64(sp)
    ld    a1, 72(sp)
    ld    a2, 80(sp)
    ld    a3, 88(sp)
    ld    a4, 96(sp)
    ld    a5,104(sp)
    ld    a6,112(sp)
    ld    a7,120(sp)
    addi  sp, sp, 144
    ld    t0, 0(sp)
    ld    t1, 8(sp)
    ld    t2,16(sp)
    ld    t3,24(sp)
    ld    t4,32(sp)
    addi  sp, sp, 48
    j     libos_syscall_entry
.size libos_syscall_entry_patched, .-libos_syscall_entry_patched

.global _return_from_syscall
.type _return_from_syscall, @function
.balign 4
_return_from_syscall:
    /* Expects one argument (in a0): pointer to PAL_CONTEXT.
     *
     * Every return is an exact restore done by the PAL (pal_arch_restore_context() in
     * pal/src/host/linux/arch/riscv64/pal_arch_exception.c): it hands the context to the kernel
     * through rt_sigreturn(), which installs all registers and the pc at once. There is no way
     * to do this in user mode without giving up a register: a riscv64 indirect jump needs one,
     * and the application expects all of them back (see the file comment).
     *
     * The cost is two plain host syscalls per emulated syscall, on top of the SIGSYS round trip
     * that brought the syscall here. */
    .cfi_startproc
    tail  pal_arch_restore_context@plt
    .cfi_endproc
.size _return_from_syscall, .-_return_from_syscall

.section .note.GNU-stack,"",@progbits
