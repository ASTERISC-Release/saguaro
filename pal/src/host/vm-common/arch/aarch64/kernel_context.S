/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2023 Intel Corporation */

/*
 * aarch64 kernel entry/exit code -- the counterpart of vm-common/kernel_events.S.
 *
 * Register design (see arch/aarch64/kernel_multicore.h for the full rationale):
 *   TPIDR_EL1            -> struct per_cpu_data (x86 GS_KERNEL_BASE); written once per CPU.
 *   per_cpu_data->cur_tcb -> current thread's struct pal_tcb_vm (x86 GS_BASE role); updated by
 *                           save_context_and_restore_next() on every switch and read by the
 *                           `svc` dispatcher below (x86: hardware loads GS with the TCB base on
 *                           every ring3->ring0 transition, we do it with two loads).
 *   TPIDR_EL0            -> the *user* TLS register (x86 FS_BASE); swapped in C
 *                           (kernel_sched.c / el0_irq asm), never relied on by the kernel.
 *
 * Syscall model, mirrored 1:1 from the x86 file:
 *   - `svc` enters with SP_EL0 intact and *no* automatic stack switch; we deliberately run the
 *     whole syscall path on the thread's own user stack (x86 `syscall_asm` likewise keeps the
 *     ring-3 RSP -- see the "FIXME: GS is not used by ring-3" comments there). This is what
 *     makes cooperative preemption work: a context switch then saves/restores `context.sp`
 *     pointing into that thread's own stack, never a shared per-CPU one.
 *   - the EL1 stack (SP_EL1) is only ever valid *after* an entry wrote it; every EL1 handler
 *     (el0_irq/el1_sync/el1_irq, landing with B2) sets SP_EL1 from
 *     per_cpu_data->interrupt_stack as its first action and leaves it clobbered afterwards.
 *   - EL0 -> EL1 `svc` does not save any user GPRs: LibOS saves what it needs in its own TCB
 *     (Milestone A's contract), the kernel only records the resume address in
 *     context.user_rip (x86: %rcx -> PAL_TCB_VM_USER_RIP).
 *   - the *return* trampoline (pal_syscall_return / pal_sigreturn_return) is handed to LibOS in
 *     x16 exactly like x86 passes sysret_asm/sigreturn_asm in %rcx; LibOS's epilogue does
 *     `br x16` with the syscall result in x0.
 *   - preemption by a timer IRQ from EL0 re-enters userland via isr_eret_to_userland reading
 *     thread->irq_pseudo_stack -- the exact analog of isr_iret_to_userland + iretq popping the
 *     iret frame.
 *
 * B0 scope: vectors table branches (el0_sync, context switch, eret paths, thread_main_wrapper)
 * are final; the IRQ frame-save entries (el0_irq/el1_sync/el1_irq) trap to `hang` until the
 * B2 interrupt bring-up lands -- interrupts_init() must not unmask DAIF.I before then.
 */

#include "asm-offsets.h"

    .macro globl sym
    .globl \sym
    .hidden \sym
    .endm

    .text

/* ---------------------------------------------------------------------------
 * Exception vector table (VBAR_EL1), installed by syscalls_init().
 * Four blocks of 128-byte entries: Current-EL-with-SP_EL0 / SP_EL1, EL0-AArch64,
 * EL0-AArch32. We run the kernel on SP_EL1; user exceptions use the EL0 blocks.
 * --------------------------------------------------------------------------- */
    .balign 2048
    globl vectors
vectors:
    .balign 128
    b       hang                  /* Current EL (SP_EL0) sync: not used (we always run SP_EL1) */
    .balign 128
    b       hang                  /* Current EL (SP_EL0) IRQ: cannot happen (SP_EL1 in use) */
    .balign 128
    b       hang                  /* Current EL (SP_EL0) FIQ */
    .balign 128
    b       hang                  /* Current EL (SP_EL0) SError */

    .balign 128
    b       el1_sync_entry        /* Current EL (SP_EL1) sync -- kernel panic paths (B2) */
    .balign 128
    b       el1_irq_entry         /* Current EL (SP_EL1) IRQ  -- IRQs during syscalls (B2) */
    .balign 128
    b       hang                  /* Current EL (SP_EL1) FIQ */
    .balign 128
    b       hang                  /* Current EL (SP_EL1) SError */

    .balign 128
    b       el0_sync_entry        /* EL0 sync -- `svc` syscall dispatch (final, see below) */
    .balign 128
    b       el0_irq_entry         /* EL0 IRQ  -- preemptive timer/device interrupts (B2) */
    .balign 128
    b       hang                  /* EL0 FIQ */
    .balign 128
    b       hang                  /* EL0 SError */

    .balign 128
    b       hang                  /* EL0 AArch32 sync -- AArch32 guests unsupported */
    .balign 128
    b       hang
    .balign 128
    b       hang
    .balign 128
    b       hang

hang:
    b       hang

/* ---------------------------------------------------------------------------
 * EL0 `svc` entry -- the aarch64 mirror of x86 syscall_asm:
 *
 *   mov %rcx, %gs:PAL_TCB_VM_USER_RIP          -> str (elr+4) into context.user_rip
 *   lea sysret_asm(%rip), %rcx  (or sigreturn) -> x16 = return trampoline
 *   jmp *%gs:(PAL_TCB_LIBOS + 0x8)             -> br via cur_tcb->common.libos_tcb+0x8
 *
 * Only x0-x8 are sacred (Linux aarch64 syscall ABI: x8 = syscall number, x0-x5 args); all
 * restorable scratch work happens in x9-x18. The EL1 stack is switched to the user stack
 * here (see the file comment) before entering LibOS code.
 * --------------------------------------------------------------------------- */
    .balign 128
el0_sync_entry:
    /* Only `svc` (EC=0x15) has an EL0->EL1 service path. Any other EL0 synchronous exception
     * (undefined instruction, data/instruction abort from a wild access, BRK/BKPT, ...) must NOT
     * be misdispatched as a syscall: without this filter an instruction-fetch abort (e.g. a
     * non-PIE binary linked below the DRAM base, where the identity map reaches only into the
     * physical hole) reports EC=0 at ELR of the very first app instruction, and blindly
     * "returning from the syscall" at ELR+4 walks faulting one instruction at a time forever.
     * Full signal delivery: .Lel0_sync_unexpected builds the full el0_irq-shaped frame and
     * forwards it through the LibOS memfault path (x86 #PF parity); only ECs LibOS does not
     * service (e.g. EC=0x00 `udf`, whose x86 #UD twin is likewise not forwarded) panic. */
    mrs     x16, esr_el1
    ubfx    x16, x16, #26, #6                      /* EC */
    cmp     x16, #0x15                             /* SVC64 from AArch64 EL0 */
    b.ne    .Lel0_sync_unexpected
    /* Resume address. Unlike x86 `syscall` (whose hardware stores the post-syscall RIP in RCX
     * for us to consume), AArch64 sets ELR_EL1 itself to the address of the instruction AFTER
     * the `svc` for SVC exceptions -- exactly the semantics of `eret`, which does not return a
     * link the way sysretq consumes one. An `add x16, x16, #4` copied from the x86 mental model
     * therefore makes every syscall return to `svc + 8`: the first victim in the first test was
     * glibc's `adrp x2, <__curbrk>` between brk's `svc` and the `str x0, [x2, #1664]` that
     * stores the brk() result, which stored through a stale x2 = garbage (ELR=app+0x182b8 with
     * FAR in the EL1-only device window). Linux's aarch64 syscall path likewise does not
     * increment ELR anywhere; the "skip the svc" duty is hardware's. */
    msr     spsr_el1, x9                      /* park the app's x9 (see snapshot comment) */
    mrs     x16, elr_el1                      /* address right after the 4-byte `svc` */
    mrs     x9, tpidr_el1
    ldr     x9, [x9, #PER_CPU_DATA_CUR_TCB]   /* x86 %gs base, two loads instead */
    cbz     x9, hang                          /* svc with no thread context: cannot happen */
    str     x16, [x9, #PAL_TCB_VM_USER_RIP]
    mrs     x16, spsr_el1                     /* x16 is snapshot store scratch from here on */
    str     x16, [x9, #PAL_TCB_VM_SYSCALL_SCRATCH + 8 * 8]
    /* Snapshot the registers the Linux syscall ABI hands back to EL0. The LibOS dispatcher
     * is C at EL1 (all of x0..x18 are C-caller-saved), so x1..x15/x17/x18 would otherwise die
     * in it -- Linux takes a full pt_regs snapshot per svc and hides this; x86 needs no
     * equivalent because the x86 dispatcher never leaves user values in registers the C ABI
     * does not preserve (x0..x15 there are exactly the kernel-clobbered set). x0 is the
     * result slot (passed through), x16 stays kernel-owned scratch like x86's sysret rcx
     * clobber that glibc declares dead. The app's x9 is the only awkward one: x9 becomes
     * the TCB store base below, so its user value is parked in SPSR_EL1 *before* the `mrs`
     * clobbers it and read back right after the TCB load. That parking spot is safe: the
     * entry-time SPSR is never read back by anything (pal_syscall_return overwrites it with
     * its eret value, pal_sigreturn_return with the frame's) and exception entry to EL1
     * hardware-masks IRQs, so nothing can touch it between park and readback. Slot layout:
     * x1..x15 -> 0..14 (slot 8 = the app's x9), x18 -> 15, x17 -> 16 -- the exact set the
     * Linux syscall ABI preserves across
     * `svc`, which pal_syscall_return's ldp pairs read back. glibc on aarch64 *pins the
     * tunable_list pointer in x18* for the whole process (static-pie startup materializes it
     * with `mov x18,#0` + `movk x18,#0x28`-style immediates and later reads slots with
     * `ldr x8, [x18, #200]` across syscalls without reloading -- the crash that dictated
     * this slot), the heap-poison pattern reuses x1, and glibc's clone3/clone child paths
     * call fn with x11/x12 inherited from the parent across the svc. An off-by-one here
     * (dropping x9 and shifting x10..x15 down a slot) is what made the clone child's
     * `blr x11` jump to 0: the kernel's `slot[k]` must be exactly `x(k+1)` for all k. */
    str     x1,  [x9, #PAL_TCB_VM_SYSCALL_SCRATCH + 8 * 0]
    str     x2,  [x9, #PAL_TCB_VM_SYSCALL_SCRATCH + 8 * 1]
    str     x3,  [x9, #PAL_TCB_VM_SYSCALL_SCRATCH + 8 * 2]
    str     x4,  [x9, #PAL_TCB_VM_SYSCALL_SCRATCH + 8 * 3]
    str     x5,  [x9, #PAL_TCB_VM_SYSCALL_SCRATCH + 8 * 4]
    str     x6,  [x9, #PAL_TCB_VM_SYSCALL_SCRATCH + 8 * 5]
    str     x7,  [x9, #PAL_TCB_VM_SYSCALL_SCRATCH + 8 * 6]
    str     x8,  [x9, #PAL_TCB_VM_SYSCALL_SCRATCH + 8 * 7]
    str     x10, [x9, #PAL_TCB_VM_SYSCALL_SCRATCH + 8 * 9]
    str     x11, [x9, #PAL_TCB_VM_SYSCALL_SCRATCH + 8 * 10]
    str     x12, [x9, #PAL_TCB_VM_SYSCALL_SCRATCH + 8 * 11]
    str     x13, [x9, #PAL_TCB_VM_SYSCALL_SCRATCH + 8 * 12]
    str     x14, [x9, #PAL_TCB_VM_SYSCALL_SCRATCH + 8 * 13]
    str     x15, [x9, #PAL_TCB_VM_SYSCALL_SCRATCH + 8 * 14]
    str     x18, [x9, #PAL_TCB_VM_SYSCALL_SCRATCH + 8 * 15]
    str     x17, [x9, #PAL_TCB_VM_SYSCALL_SCRATCH + 8 * 16]

    cmp     x8, #SYSCALL_RT_SIGRETURN
    adrp    x16, pal_syscall_return
    add     x16, x16, :lo12:pal_syscall_return
    b.ne    .Lsvc_libos
    adrp    x16, pal_sigreturn_return
    add     x16, x16, :lo12:pal_sigreturn_return
.Lsvc_libos:
    /* run the syscall on the thread's own user stack (x86: rsp never left ring-3 stack) */
    mrs     x17, sp_el0
    mov     sp, x17
    ldr     x9, [x9, #PAL_TCB_LIBOS]          /* struct libos_tcb embedded in the PAL TCB */
    cbz     x9, hang                          /* LibOS not initialized on this thread yet */
    ldr     x9, [x9, #8]                      /* common.libos_syscall_entry (LibOS wrote it) */
    cbz     x9, hang
    br      x9

.Lel0_sync_unexpected:
    /* EL0 fault (not `svc`): build the exact `struct isr_regs` frame that
     * el0_irq_entry builds and hand it to the C handler, which forwards it
     * through pal_common_perform_memfault_handling -- the same entry point the
     * x86 #PF/#GP ISRs use (kernel_interrupts.c isr_c). LibOS's memfault upcall
     * either fixes the fault in place (VMA bookkeeping) or rewrites the frame to
     * deliver SIGSEGV/SIGBUS to the app's own handler; on success we resume EL0
     * from the (possibly rewritten) frame via `eret`, exactly like el0_irq's
     * same-thread return. Without a registered handler C reports the full
     * diagnostic and stops (x86 #PF panic parity).
     *
     * Bootstrap is the el0_irq_entry one: the exception left no scratch GPR and
     * all 31 must survive, so 48B below the *incoming* SP_EL1 (always a mapped
     * kernel-or-user-syscall stack -- the last one EL1 code used) parks x29/x30
     * while the frame is carved at the per-CPU interrupt stack. Trusting
     * SP_EL0 for anything would be wrong on precisely this path: the fault may
     * be a stack-guard page. */
    sub     sp, sp, #48
    stp     x29, x30, [sp]
    mrs     x29, elr_el1
    mrs     x30, spsr_el1
    stp     x29, x30, [sp, #16]
    mrs     x29, sp_el0
    mrs     x30, tpidr_el1
    ldr     x30, [x30, #PER_CPU_DATA_INTERRUPT_STACK]
    sub     x30, x30, #ISR_FRAME_SIZE
    str     x29, [x30, #ISR_SP]
    ldr     x29, [sp]
    str     x29, [x30, #ISR_REGS + 29 * 8]
    ldr     x29, [sp, #16]
    str     x29, [x30, #ISR_ELR]
    ldr     x29, [sp, #24]
    str     x29, [x30, #ISR_SPSR]
    ldr     x29, [sp, #8]
    str     x29, [x30, #ISR_REGS + 30 * 8]
    mrs     x29, tpidr_el1
    ldr     x29, [x29, #PER_CPU_DATA_INTERRUPT_FPSIMD_AREA]
    str     x29, [x30, #ISR_FPREGS]
    stp     x0,  x1,  [x30, #ISR_REGS + 0]
    stp     x2,  x3,  [x30, #ISR_REGS + 16]
    stp     x4,  x5,  [x30, #ISR_REGS + 32]
    stp     x6,  x7,  [x30, #ISR_REGS + 48]
    stp     x8,  x9,  [x30, #ISR_REGS + 64]
    stp     x10, x11, [x30, #ISR_REGS + 80]
    stp     x12, x13, [x30, #ISR_REGS + 96]
    stp     x14, x15, [x30, #ISR_REGS + 112]
    stp     x16, x17, [x30, #ISR_REGS + 128]
    stp     x18, x19, [x30, #ISR_REGS + 144]
    stp     x20, x21, [x30, #ISR_REGS + 160]
    stp     x22, x23, [x30, #ISR_REGS + 176]
    stp     x24, x25, [x30, #ISR_REGS + 192]
    stp     x26, x27, [x30, #ISR_REGS + 208]
    stp     x28, x29, [x30, #ISR_REGS + 224]
    mov     sp, x30

    ldr     x0, [sp, #ISR_FPREGS]
    bl      do_xsave                      /* the signal ucontext sees live FP state */
    mov     x0, sp
    bl      el0_sync_unexpected_c         /* 0 = frame rewritten, resume from it */
    cbnz    x0, hang

    /* Resume EL0 from the (possibly rewritten) frame: eret consumes ELR/SPSR from
     * the sysregs and SP_EL0 for the EL0t target, so all three are reinstalled
     * here -- unlike el0_irq (whose C dispatcher cannot touch them), the memfault
     * upcall may have moved pc/sp on purpose. x16/x17 are frame slots like any
     * other, restored last from the still-live frame base. */
    ldr     x16, [sp, #ISR_ELR]
    msr     elr_el1, x16
    ldr     x16, [sp, #ISR_SPSR]
    msr     spsr_el1, x16
    ldr     x16, [sp, #ISR_SP]
    msr     sp_el0, x16
    ldp     x0,  x1,  [sp, #ISR_REGS + 0]
    ldp     x2,  x3,  [sp, #ISR_REGS + 16]
    ldp     x4,  x5,  [sp, #ISR_REGS + 32]
    ldp     x6,  x7,  [sp, #ISR_REGS + 48]
    ldp     x8,  x9,  [sp, #ISR_REGS + 64]
    ldp     x10, x11, [sp, #ISR_REGS + 80]
    ldp     x12, x13, [sp, #ISR_REGS + 96]
    ldp     x14, x15, [sp, #ISR_REGS + 112]
    ldp     x18, x19, [sp, #ISR_REGS + 144]
    ldp     x20, x21, [sp, #ISR_REGS + 160]
    ldp     x22, x23, [sp, #ISR_REGS + 176]
    ldp     x24, x25, [sp, #ISR_REGS + 192]
    ldp     x26, x27, [sp, #ISR_REGS + 208]
    ldp     x28, x29, [sp, #ISR_REGS + 224]
    ldr     x30, [sp, #ISR_REGS + 240]
    ldr     x17, [sp, #ISR_REGS + 136]
    ldr     x16, [sp, #ISR_REGS + 128]
    eret

/* ---------------------------------------------------------------------------
 * void pal_syscall_return(void) -- x86 sysret_asm analog, entered via `br x16`
 * from the LibOS syscall epilogue with the syscall result in x0.
 *
 * SP_EL0 comes from the current EL1 SP: the LibOS epilogue switched to the thread's saved user
 * stack (`mov sp, regs->sp`) before branching here, and that stack *is* the thread's SP_EL0
 * value at svc time (the whole syscall ran on it, x86's RSP-never-leaves-ring-3 model), so the
 * per-CPU SP_EL0 sysreg is refreshed to *this* thread's stack right before the eret. SPSR for
 * the eret is 0: EL0t with all DAIF masks cleared (x86: RFLAGS |= IF).
 * The app's live scratch registers come back from the entry-side snapshot (see the
 * syscall_scratch note in kernel_thread.h): x1 is *not* usable as the ELR scratch
 * here, because glibc's ptmalloc_init poisons the heap with `svc (getrandom);
 * add x19, sp, x1` -- reusing the preserved length argument -- and storing the
 * tunables output through x19 = sp + <the resume address we put in x1> produced a
 * wild write (permission fault, FAR = stack + <code address>). x16/x17 are the
 * allowed clobbers (x86 sysret rcx/r11 parity).
 * --------------------------------------------------------------------------- */
    globl pal_syscall_return
pal_syscall_return:
    mrs     x16, tpidr_el1
    ldr     x16, [x16, #PER_CPU_DATA_CUR_TCB]
    ldr     x17, [x16, #PAL_TCB_VM_USER_RIP]
    msr     elr_el1, x17                      /* park the resume address in the sysreg: even
                                                 x17 must come back from the frame (only the
                                                 user x16 dies here, as the entry path's adrp
                                                 scratch -- x86 sysret %rcx parity) */
    msr     spsr_el1, xzr
    /* SP_EL0 is a *per-CPU* sysreg, not per-thread: whenever any thread eret'd to EL0 or was
     * preempted, it left its own user stack in it. With more than one EL0 thread the resumed
     * thread would run with the last-switched-out thread's SP_EL0. Here the EL1 SP *is* the
     * resumed thread's user stack (_return_from_syscall does `mov sp, regs->sp` before its
     * `br` into this trampoline), so installing it is both required and correct (Linux does
     * the same dance in its ret-to-userspace path). */
    mov     x17, sp
    msr     sp_el0, x17
    add     x17, x16, #PAL_TCB_VM_SYSCALL_SCRATCH
    ldp     x1,  x2,  [x17, #8 * 0]
    ldp     x3,  x4,  [x17, #8 * 2]
    ldp     x5,  x6,  [x17, #8 * 4]
    ldp     x7,  x8,  [x17, #8 * 6]
    ldp     x9,  x10, [x17, #8 * 8]
    ldp     x11, x12, [x17, #8 * 10]
    ldp     x13, x14, [x17, #8 * 12]
    ldr     x15, [x17, #8 * 14]
    ldr     x18, [x17, #8 * 15]
    ldr     x17, [x17, #8 * 16]               /* frame base's own slot, restored last */
    eret

/* ---------------------------------------------------------------------------
 * void pal_sigreturn_return(void) -- x86 sigreturn_asm analog, reached via
 * `br x16` from _return_from_syscall (LibOS's VM/TDX branch of
 * libos_syscall_rt_sigreturn() points context.pc back at this trampoline after
 * copying the restored app RIP into TCB.user_rip -- the same planting x86 does).
 *
 * Unlike x86 there is no iret frame to assemble: eret consumes the sysregs
 * directly. x86 must run on a separate pseudo-stack (sigreturn_pseudo_rsp)
 * because building the iret frame clobbers the user RSP it is about to resume
 * with; aarch64 sysreg writes (msr) go through registers only, so the
 * resume-{pc,sp,regs} triple is already exactly where it needs to be:
 *   - ELR:      TCB.user_rip, planted by LibOS (the fault-time pc, +handler edits)
 *   - SP_EL0:   the live EL1 SP -- _return_from_syscall did `mov sp, regs->sp`
 *               with the *restored* app stack before branching here
 *   - GPRs/x0:  all live from the same restore (x0 = the app's fault-time x0,
 *               the analog of x86 keeping user rax through the pseudo-stack dance)
 * SPSR = 0: EL0t, DAIF clear (x86: iret frame RFLAGS |= IF).
 * x16/x17 are the only scratch (the app's true values are what the restore put
 * in them -- clobbering x16 to read the TCB is fine, x17 likewise; sigreturn's
 * contract is the saved ucontext, and the syscall ABI declares both dead). */
    globl pal_sigreturn_return
pal_sigreturn_return:
    mrs     x16, tpidr_el1
    ldr     x16, [x16, #PER_CPU_DATA_CUR_TCB]
    ldr     x17, [x16, #PAL_TCB_VM_USER_RIP]   /* LibOS-planted restored pc */
    msr     elr_el1, x17
    msr     spsr_el1, xzr
    mov     x17, sp                            /* restored user stack (per-CPU SP_EL0
                                                  refresh, same requirement as pal_syscall_return) */
    msr     sp_el0, x17
    eret

/* ---------------------------------------------------------------------------
 * void isr_eret_to_userland(void) -- x86 isr_iret_to_userland analog.
 *
 * The preemptive-return tail: save_context_and_restore_next() restored x19-x30
 * (user values: context.regs[19..30] was written from the el0_irq frame by
 * save_userland_context), set SP_EL1 to &thread->irq_pseudo_stack and branches
 * here. From the stack we get the iret-equivalent frame {elr, spsr, sp}; user
 * x0-x18 are reloaded from context.regs (the el0_irq handler snapshot) before
 * the eret. User TLS was already written by C before the context switch.
 * --------------------------------------------------------------------------- */
    globl isr_eret_to_userland
isr_eret_to_userland:
    ldp     x1, x2, [sp]                      /* frame.elr, frame.spsr */
    ldr     x3, [sp, #16]                     /* frame.sp: resume stack pointer */
    msr     elr_el1, x1
    msr     spsr_el1, x2
    /* Two preempt sources share this tail: an EL0 thread preempted by el0_irq
     * (SPSR.M = EL0t, resume SP is the *user* stack -> SP_EL0 sysreg, and eret
     * switches which stack register SP names) and kernel code preempted by
     * el1_irq with RFLAGS-equivalent IRQs unmasked (SPSR.M = EL1h -- eret to
     * EL1 does not reload any GPR, unlike x86's iretq popping SS:RSP, so the
     * interrupted kernel SP must go back into SP itself here; the save path
     * left SP_EL1 at &irq_pseudo_stack just for this frame read). */
    tst     x2, #0xc                          /* SPSR.M[3:2] == 0 only at EL0 */
    b.ne    .Lieret_el1
    msr     sp_el0, x3
    b       .Lieret_regs
.Lieret_el1:
    mov     sp, x3
.Lieret_regs:

    mrs     x16, tpidr_el1
    ldr     x16, [x16, #PER_CPU_DATA_CUR_TCB]
    add     x17, x16, #PAL_TCB_VM_CONTEXT_REGS
    ldp     x0, x1, [x17]
    ldp     x2, x3, [x17, #16]
    ldp     x4, x5, [x17, #32]
    ldp     x6, x7, [x17, #48]
    ldp     x8, x9, [x17, #64]
    ldp     x10, x11, [x17, #80]
    ldp     x12, x13, [x17, #96]
    ldp     x14, x15, [x17, #112]
    ldr     x18, [x17, #144]
    ldr     x17, [x17, #136]
    ldr     x16, [x16, #PAL_TCB_VM_CONTEXT_REGS + 128]
    eret

/* ---------------------------------------------------------------------------
 * int save_context_and_restore_next(uint64_t curr_tcb, uint64_t next_tcb,
 *      uint32_t* lock_to_unlock, int* clear_child_tid,
 *      uint32_t* critical_section_lock_to_unlock, void* scheduling_stack)
 *
 * Cooperative kernel-thread switch (x86 save_context_and_restore_next). The x86
 * asm could clobber only caller-saved registers before saving the current
 * context because its six arguments all arrived in caller-saved registers and
 * do_xsave/do_xrstor are ABI-respecting; aarch64 arguments arrive in x0-x5 and
 * our do_xsave/do_xrstor are plain C, so the sequence is: save the current
 * context's callee-saved x19-x30/sp/pc *first* (they are still in the registers
 * on entry!), stash x1-x5 over C calls on the outgoing stack (safe: DAIF.I is
 * masked in EL1 kernel context -- only EL0 IRQs preempt, and that path passes
 * curr_tcb = 0 exactly like x86), and keep every scratch action in x6-x18.
 * --------------------------------------------------------------------------- */
    globl save_context_and_restore_next
save_context_and_restore_next:
    cbz     x0, .Lscr_restore_next

    /* ---- save current context: callee regs + sp are still live in the registers ---- */
    mrs     x6, daif
    bic     x6, x6, #0x3c0                    /* resumed context runs with IRQs enabled
                                                 (x86: pushf/popf RFLAGS |= IF) */
    str     x6, [x0, #PAL_TCB_VM_CONTEXT_PSTATE]
    /* regs[] starts at 0x160, past the ±512 ldp/stp window: use a scratch base (x7 is
     * re-saved into its context slot further below, after this block is done with it) */
    add     x7, x0, #PAL_TCB_VM_CONTEXT_REGS
    stp     x19, x20, [x7, #19 * 8]
    stp     x21, x22, [x7, #21 * 8]
    stp     x23, x24, [x7, #23 * 8]
    stp     x25, x26, [x7, #25 * 8]
    stp     x27, x28, [x7, #27 * 8]
    stp     x29, x30, [x7, #29 * 8]
    mov     x6, sp
    str     x6, [x0, #PAL_TCB_VM_CONTEXT_SP]
    str     xzr, [x0, #PAL_TCB_VM_CONTEXT_REGS]   /* resumed with x0 = 0 (success), x86 parity */
    adrp    x6, .Lscr_resume_yield
    add     x6, x6, :lo12:.Lscr_resume_yield
    str     x6, [x0, #PAL_TCB_VM_CONTEXT_PC]

    /* save x1-x18 too so the context image is a complete register snapshot (they hold
     * our own argument values here; dead for .Lscr_resume_yield, harmless elsewhere --
     * preempted-user threads are saved by save_userland_context() instead, which never
     * goes through this function (curr_tcb is 0 there, x86 parity) */
    stp     x1, x2, [x0, #PAL_TCB_VM_CONTEXT_REGS + 1 * 8]
    stp     x3, x4, [x0, #PAL_TCB_VM_CONTEXT_REGS + 3 * 8]
    stp     x5, x6, [x0, #PAL_TCB_VM_CONTEXT_REGS + 5 * 8]
    stp     x7, x8, [x0, #PAL_TCB_VM_CONTEXT_REGS + 7 * 8]
    stp     x9, x10, [x0, #PAL_TCB_VM_CONTEXT_REGS + 9 * 8]
    stp     x11, x12, [x0, #PAL_TCB_VM_CONTEXT_REGS + 11 * 8]
    stp     x13, x14, [x0, #PAL_TCB_VM_CONTEXT_REGS + 13 * 8]
    stp     x15, x16, [x0, #PAL_TCB_VM_CONTEXT_REGS + 15 * 8]
    stp     x17, x18, [x0, #PAL_TCB_VM_CONTEXT_REGS + 17 * 8]

    /* do_xsave() is C and clobbers x0-x17: stash (next_tcb, locks, sched stack) on the
     * outgoing thread's stack; this stack cannot be raced on -- we hold
     * critical_section_lock (x4) and EL1 IRQs are masked (x86: same push %rdx dance,
     * x86 could skip the others only because its do_xsave is hand-written asm) */
    stp     x1, x2, [sp, #-48]!
    stp     x3, x4, [sp, #16]
    str     x5, [sp, #32]
    ldr     x0, [x0, #PAL_TCB_VM_CONTEXT_FPREGS]
    bl      do_xsave
    ldr     x5, [sp, #32]
    ldp     x3, x4, [sp, #16]
    ldp     x1, x2, [sp], #48

.Lscr_restore_next:
    /* x1 = next_tcb (survived: the skip-branch path touched no registers, the save path
     * reloaded it), x2-x5 likewise */
    mrs     x16, tpidr_el1
    str     x1, [x16, #PER_CPU_DATA_CUR_TCB]     /* wrmsr(MSR_IA32_GS_BASE, next_gs_base) */
    /* This very per-CPU slot IS the EL0-readable TCB cell: each CPU's TPIDRRO_EL0 (EL0-read-
     * only, set once at boot/AP-startup -- see vm_bootloader.S) holds &per_cpu_data[cpu].
     * cur_tcb, so the store above simultaneously performs the x86 "make gsbase ring-3
     * readable" mirror with no second memory location, no second write, and no possibility
     * of a half-updated pair. (The former global g_pal_tcb_singleton mirror broke on SMP:
     * another CPU's switch would write its own thread -- or NULL parking an AP boot context
     * -- into the cell this CPU's EL0 pal_get_tcb() was about to read.)
     */

    cbz     x2, .Lscr_unlock_done
    str     wzr, [x2]                            /* spinlock_unlock(lock_to_unlock) */
.Lscr_unlock_done:
    cbz     x3, .Lscr_child_done
    str     wzr, [x3]                            /* *clear_child_tid = 0 (parent futex-wake
                                                    side is LibOS's, done at el0 return) */
.Lscr_child_done:
    /* move to the per-CPU "scheduling" stack before unlocking the critical section, so
     * that no CPU can still touch the outgoing thread's stack underneath us (x86 comment
     * block applies verbatim; scheduling_stack is NULL from interrupt context) */
    cbz     x5, .Lscr_stack_done
    mov     sp, x5
.Lscr_stack_done:
    str     wzr, [x4]                            /* leave the critical section */

    ldr     x0, [x1, #PAL_TCB_VM_CONTEXT_FPREGS]
    bl      do_xrstor                            /* C: clobbers x1 too, reload below */

    /* re-find the next TCB via cur_tcb (we stored it before the C calls) */
    mrs     x16, tpidr_el1
    ldr     x1, [x16, #PER_CPU_DATA_CUR_TCB]

    ldr     x6, [x1, #PAL_TCB_VM_CONTEXT_PSTATE]
    msr     daif, x6                             /* popfq parity: may unmask IRQs only now */

    /* base for regs[] (x16 is reloaded from cur_tcb afterwards; see the do_xrstor note) */
    add     x16, x1, #PAL_TCB_VM_CONTEXT_REGS
    ldp     x19, x20, [x16, #19 * 8]
    ldp     x21, x22, [x16, #21 * 8]
    ldp     x23, x24, [x16, #23 * 8]
    ldp     x25, x26, [x16, #25 * 8]
    ldp     x27, x28, [x16, #27 * 8]
    ldp     x29, x30, [x16, #29 * 8]
    ldr     x16, [x1, #PAL_TCB_VM_CONTEXT_PC]    /* .Lscr_resume_yield or thread_main_wrapper */
    ldr     x0, [x1, #PAL_TCB_VM_CONTEXT_REGS]   /* x86 movq CONTEXT_RAX,%rax parity */
    /* thread_main_wrapper's first start takes x1 = callback, x2 = terminate_func from
     * regs[1]/regs[2] (thread_setup wrote them; x86 parity: wrapper args arrive in rsi/rdx
     * from the restored frame) -- x1 held the TCB base here and regs[1] was never loaded,
     * so freshly created threads `blr`'d into the TCB itself. Resumed (.Lscr_resume_yield)
     * threads never read x1/x2, so clobbering them is harmless there. Load through x1 last. */
    ldr     x2, [x1, #PAL_TCB_VM_CONTEXT_REGS + 16]
    ldr     x6, [x1, #PAL_TCB_VM_CONTEXT_SP]
    ldr     x1, [x1, #PAL_TCB_VM_CONTEXT_REGS + 8]
    mov     sp, x6
    br      x16

.Lscr_resume_yield:
    ret                                          /* back into sched_thread(), x0 = 0 */

/* ---------------------------------------------------------------------------
 * void thread_main_wrapper(void* callback_args, void* callback,
 *                          void* terminate_func)
 *
 * x0/x1/x2 arrive via context.regs[0..2] (thread_setup wrote them) and a `br`
 * (no link register), same as x86's jmp into the wrapper. Supports thread
 * callbacks that `return` instead of calling PalThreadExit(); without this the
 * thread would jump off the stack upon return.
 * --------------------------------------------------------------------------- */
    globl thread_main_wrapper
thread_main_wrapper:
    stp     x2, xzr, [sp, #-32]!                 /* terminate_func across the C call below */
    blr     x1                                   /* callback(callback_args) */
    ldr     x2, [sp]
    mov     x0, xzr                              /* exit status 0, x86 `xor %rdi,%rdi` parity */
    blr     x2                                   /* pal_common_thread_exit(0) -- noreturn */
    udf    #0

/* ---------------------------------------------------------------------------
 * EL1 exception entries: the aarch64 counterpart of vm-common/kernel_events.S's
 * `irq_x` stubs + isr_c dispatch. Frame layout is struct isr_regs
 * (vm-common/kernel_interrupts.h):
 *      +0   fpregs   (per-CPU FP/SIMD save area)
 *      +8   regs[31] (x0..x30)
 *      +256 elr     +264 spsr      +272 sp (the stack eret/mov restores)
 * sizeof = 280, the frame is rounded to 288 so SP stays 16B-aligned.
 *
 * SP management. x86 hardware pushes the iret frame on a stack that the CPU
 * switches to via the TSS IST entry; aarch64 switches no stack at all. We use
 * the per-CPU `interrupt_stack` (top stored in per_cpu_data->interrupt_stack,
 * grows down). The bootstrap cannot read ELR/SPSR/SP_EL0/TPIDR without a
 * scratch GPR, and all 31 GPRs must survive, so -- exactly like the Linux
 * entry macros -- the bootstrap first carves 48B of scratch below the *incoming*
 * SP and stashes x29/x30 there. That write is safe: the incoming SP_EL1 always
 * belongs to a mapped stack (EL0 IRQ: wherever EL1 code last left it -- some
 * kernel or user-syscall stack; EL1 IRQ: the stack of the interrupted kernel
 * code) and memory below SP is dead space of it. IRQs cannot nest here: the
 * exception entry itself masks DAIF.I and no handler C code unmasks it.
 * --------------------------------------------------------------------------- */
    .equ ISR_FRAME_SIZE,  288             /* sizeof(struct isr_regs) rounded up  */
    .equ ISR_FPREGS,      0
    .equ ISR_REGS,        8               /* regs[31]: reg N at ISR_REGS + 8*N   */
    .equ ISR_ELR,         256
    .equ ISR_SPSR,        264
    .equ ISR_SP,          272             /* SP to resume (user SP_EL0 or kernel)*/

    .balign 128
el0_irq_entry:
    /* IRQ taken while the CPU ran at EL0 (x86: irq_x from ring 3). Once the
     * dispatcher returns we always go back to the *same* thread -- a context
     * switch taken by sched_thread_uninterruptable() leaves through
     * isr_eret_to_userland() and never returns here (x86: the preempted ISR
     * likewise never executed its iret). */
    sub     sp, sp, #48
    stp     x29, x30, [sp]
    mrs     x29, elr_el1
    mrs     x30, spsr_el1
    stp     x29, x30, [sp, #16]
    mrs     x29, sp_el0                   /* the preempted user stack: the SP that */
    mrs     x30, tpidr_el1                /* eret resumes with (SP_EL0 sysreg) */
    ldr     x30, [x30, #PER_CPU_DATA_INTERRUPT_STACK]
    sub     x30, x30, #ISR_FRAME_SIZE
    str     x29, [x30, #ISR_SP]
    ldr     x29, [sp]
    str     x29, [x30, #ISR_REGS + 29 * 8]
    ldr     x29, [sp, #16]
    str     x29, [x30, #ISR_ELR]
    ldr     x29, [sp, #24]
    str     x29, [x30, #ISR_SPSR]
    ldr     x29, [sp, #8]
    str     x29, [x30, #ISR_REGS + 30 * 8]
    mrs     x29, tpidr_el1
    ldr     x29, [x29, #PER_CPU_DATA_INTERRUPT_FPSIMD_AREA]
    str     x29, [x30, #ISR_FPREGS]
    stp     x0,  x1,  [x30, #ISR_REGS + 0]
    stp     x2,  x3,  [x30, #ISR_REGS + 16]
    stp     x4,  x5,  [x30, #ISR_REGS + 32]
    stp     x6,  x7,  [x30, #ISR_REGS + 48]
    stp     x8,  x9,  [x30, #ISR_REGS + 64]
    stp     x10, x11, [x30, #ISR_REGS + 80]
    stp     x12, x13, [x30, #ISR_REGS + 96]
    stp     x14, x15, [x30, #ISR_REGS + 112]
    stp     x16, x17, [x30, #ISR_REGS + 128]
    stp     x18, x19, [x30, #ISR_REGS + 144]
    stp     x20, x21, [x30, #ISR_REGS + 160]
    stp     x22, x23, [x30, #ISR_REGS + 176]
    stp     x24, x25, [x30, #ISR_REGS + 192]
    stp     x26, x27, [x30, #ISR_REGS + 208]
    stp     x28, x29, [x30, #ISR_REGS + 224]
    mov     sp, x30                       /* sp = frame (x30's user value is saved) */

    ldr     x0, [sp, #ISR_FPREGS]
    bl      do_xsave                      /* preempted thread's V0-V31 + FPCR/FPSR */
    mov     x0, sp
    bl      isr_c                         /* ack GIC, tick, maybe sched; may not return */

    /* Same-thread return: eret consumes ELR/SPSR from sysregs; the user SP is
     * already in SP_EL0 (we never touched it) and the user GPRs come back from
     * the frame. x16 (a frame slot like all GPRs) is restored last. */
    ldr     x16, [sp, #ISR_ELR]
    msr     elr_el1, x16
    ldr     x16, [sp, #ISR_SPSR]
    msr     spsr_el1, x16
    ldp     x0,  x1,  [sp, #ISR_REGS + 0]
    ldp     x2,  x3,  [sp, #ISR_REGS + 16]
    ldp     x4,  x5,  [sp, #ISR_REGS + 32]
    ldp     x6,  x7,  [sp, #ISR_REGS + 48]
    ldp     x8,  x9,  [sp, #ISR_REGS + 64]
    ldp     x10, x11, [sp, #ISR_REGS + 80]
    ldp     x12, x13, [sp, #ISR_REGS + 96]
    ldp     x14, x15, [sp, #ISR_REGS + 112]
    ldp     x18, x19, [sp, #ISR_REGS + 144]
    ldp     x20, x21, [sp, #ISR_REGS + 160]
    ldp     x22, x23, [sp, #ISR_REGS + 176]
    ldp     x24, x25, [sp, #ISR_REGS + 192]
    ldp     x26, x27, [sp, #ISR_REGS + 208]
    ldp     x28, x29, [sp, #ISR_REGS + 224]
    ldr     x30, [sp, #ISR_REGS + 240]
    ldr     x17, [sp, #ISR_REGS + 136]
    ldr     x16, [sp, #ISR_REGS + 128]
    eret

    .balign 128
el1_irq_entry:
    /* IRQ (timer or virtio device) taken while running kernel code -- the norm
     * once interrupts_init() ran sti() (x86: the same irq_x stubs firing with
     * RFLAGS.IF=1 inside kernel C). Identical frame; the resume SP is the
     * interrupted kernel stack, and unlike eret-to-EL0 we must restore SP
     * ourselves before the eret (eret reloads no GPR, while iretq popped RSP). */
    sub     sp, sp, #48
    stp     x29, x30, [sp]
    mrs     x29, elr_el1
    mrs     x30, spsr_el1
    stp     x29, x30, [sp, #16]
    add     x29, sp, #48                  /* resume SP = the interrupted kernel stack */
    mrs     x30, tpidr_el1
    ldr     x30, [x30, #PER_CPU_DATA_INTERRUPT_STACK]
    sub     x30, x30, #ISR_FRAME_SIZE
    str     x29, [x30, #ISR_SP]
    ldr     x29, [sp]
    str     x29, [x30, #ISR_REGS + 29 * 8]
    ldr     x29, [sp, #16]
    str     x29, [x30, #ISR_ELR]
    ldr     x29, [sp, #24]
    str     x29, [x30, #ISR_SPSR]
    ldr     x29, [sp, #8]
    str     x29, [x30, #ISR_REGS + 30 * 8]
    mrs     x29, tpidr_el1
    ldr     x29, [x29, #PER_CPU_DATA_INTERRUPT_FPSIMD_AREA]
    str     x29, [x30, #ISR_FPREGS]
    stp     x0,  x1,  [x30, #ISR_REGS + 0]
    stp     x2,  x3,  [x30, #ISR_REGS + 16]
    stp     x4,  x5,  [x30, #ISR_REGS + 32]
    stp     x6,  x7,  [x30, #ISR_REGS + 48]
    stp     x8,  x9,  [x30, #ISR_REGS + 64]
    stp     x10, x11, [x30, #ISR_REGS + 80]
    stp     x12, x13, [x30, #ISR_REGS + 96]
    stp     x14, x15, [x30, #ISR_REGS + 112]
    stp     x16, x17, [x30, #ISR_REGS + 128]
    stp     x18, x19, [x30, #ISR_REGS + 144]
    stp     x20, x21, [x30, #ISR_REGS + 160]
    stp     x22, x23, [x30, #ISR_REGS + 176]
    stp     x24, x25, [x30, #ISR_REGS + 192]
    stp     x26, x27, [x30, #ISR_REGS + 208]
    stp     x28, x29, [x30, #ISR_REGS + 224]
    mov     sp, x30

    ldr     x0, [sp, #ISR_FPREGS]
    bl      do_xsave                      /* kernel C never touches FP, so this records
                                             the user FP state the interrupted thread owns */
    mov     x0, sp
    bl      isr_c

    /* Restore order: the resume SP goes back first (into SP, the register eret
     * won't fix); the frame itself lives at a per-CPU-fixed address, so the
     * remaining GPRs are re-read from there through TPIDR_EL1 after the switch. */
    ldr     x16, [sp, #ISR_ELR]
    msr     elr_el1, x16
    ldr     x16, [sp, #ISR_SPSR]
    msr     spsr_el1, x16
    ldr     x16, [sp, #ISR_SP]
    mov     sp, x16                       /* back on the interrupted kernel stack */
    mrs     x16, tpidr_el1
    ldr     x16, [x16, #PER_CPU_DATA_INTERRUPT_STACK]
    sub     x16, x16, #ISR_FRAME_SIZE     /* the frame we just left */
    ldp     x0,  x1,  [x16, #ISR_REGS + 0]
    ldp     x2,  x3,  [x16, #ISR_REGS + 16]
    ldp     x4,  x5,  [x16, #ISR_REGS + 32]
    ldp     x6,  x7,  [x16, #ISR_REGS + 48]
    ldp     x8,  x9,  [x16, #ISR_REGS + 64]
    ldp     x10, x11, [x16, #ISR_REGS + 80]
    ldp     x12, x13, [x16, #ISR_REGS + 96]
    ldp     x14, x15, [x16, #ISR_REGS + 112]
    ldp     x18, x19, [x16, #ISR_REGS + 144]
    ldp     x20, x21, [x16, #ISR_REGS + 160]
    ldp     x22, x23, [x16, #ISR_REGS + 176]
    ldp     x24, x25, [x16, #ISR_REGS + 192]
    ldp     x26, x27, [x16, #ISR_REGS + 208]
    ldp     x28, x29, [x16, #ISR_REGS + 224]
    ldr     x30, [x16, #ISR_REGS + 240]
    ldr     x17, [x16, #ISR_REGS + 136]
    ldr     x16, [x16, #ISR_REGS + 128]
    eret

    .balign 128
el1_sync_entry:
    /* A synchronous exception inside the kernel proper is a kernel bug (x86:
     * #PF/#GP/#UD panic with no fixup). Save the frame only to report it. */
    sub     sp, sp, #48
    stp     x29, x30, [sp]
    mrs     x29, elr_el1
    mrs     x30, spsr_el1
    stp     x29, x30, [sp, #16]
    add     x29, sp, #48
    mrs     x30, tpidr_el1
    ldr     x30, [x30, #PER_CPU_DATA_INTERRUPT_STACK]
    sub     x30, x30, #ISR_FRAME_SIZE
    str     x29, [x30, #ISR_SP]
    ldr     x29, [sp]
    str     x29, [x30, #ISR_REGS + 29 * 8]
    ldr     x29, [sp, #16]
    str     x29, [x30, #ISR_ELR]
    ldr     x29, [sp, #24]
    str     x29, [x30, #ISR_SPSR]
    ldr     x29, [sp, #8]
    str     x29, [x30, #ISR_REGS + 30 * 8]
    mrs     x29, tpidr_el1
    ldr     x29, [x29, #PER_CPU_DATA_INTERRUPT_FPSIMD_AREA]
    str     x29, [x30, #ISR_FPREGS]
    stp     x0,  x1,  [x30, #ISR_REGS + 0]
    stp     x2,  x3,  [x30, #ISR_REGS + 16]
    stp     x4,  x5,  [x30, #ISR_REGS + 32]
    stp     x6,  x7,  [x30, #ISR_REGS + 48]
    stp     x8,  x9,  [x30, #ISR_REGS + 64]
    stp     x10, x11, [x30, #ISR_REGS + 80]
    stp     x12, x13, [x30, #ISR_REGS + 96]
    stp     x14, x15, [x30, #ISR_REGS + 112]
    stp     x16, x17, [x30, #ISR_REGS + 128]
    stp     x18, x19, [x30, #ISR_REGS + 144]
    stp     x20, x21, [x30, #ISR_REGS + 160]
    stp     x22, x23, [x30, #ISR_REGS + 176]
    stp     x24, x25, [x30, #ISR_REGS + 192]
    stp     x26, x27, [x30, #ISR_REGS + 208]
    stp     x28, x29, [x30, #ISR_REGS + 224]
    mov     sp, x30
    mov     x0, sp
    bl      isr_kernel_c                  /* noreturn: logs and triple_fault()s */
    udf     #0

    .section .note.GNU-stack, "", %progbits      /* the kernel never executes the stack */
