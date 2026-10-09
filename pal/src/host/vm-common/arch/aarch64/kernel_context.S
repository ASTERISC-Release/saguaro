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
    mrs     x16, elr_el1                      /* address right after the 4-byte `svc` */
    add     x16, x16, #4
    mrs     x9, tpidr_el1
    ldr     x9, [x9, #PER_CPU_DATA_CUR_TCB]   /* x86 %gs base, two loads instead */
    cbz     x9, hang                          /* svc with no thread context: cannot happen */
    str     x16, [x9, #PAL_TCB_VM_USER_RIP]

    /* run the syscall on the thread's own user stack (x86: rsp never left ring-3 stack) */
    mrs     x16, sp_el0
    mov     sp, x16

    cmp     x8, #SYSCALL_RT_SIGRETURN
    adrp    x16, pal_syscall_return
    add     x16, x16, :lo12:pal_syscall_return
    b.ne    .Lsvc_libos
    adrp    x16, pal_sigreturn_return
    add     x16, x16, :lo12:pal_sigreturn_return
.Lsvc_libos:
    ldr     x9, [x9, #PAL_TCB_LIBOS]          /* struct libos_tcb embedded in the PAL TCB */
    cbz     x9, hang                          /* LibOS not initialized on this thread yet */
    ldr     x9, [x9, #8]                      /* common.libos_syscall_entry (LibOS wrote it) */
    cbz     x9, hang
    br      x9

/* ---------------------------------------------------------------------------
 * void pal_syscall_return(void) -- x86 sysret_asm analog, entered via `br x16`
 * from the LibOS syscall epilogue with the syscall result in x0.
 *
 * SP_EL0 is untouched by everything we ran (svc doesn't change it and the kernel
 * only ever used SP_EL1 for its bookkeeping, with the exception of the deliberate
 * "kernel runs on user stack" switch, which is balanced by C's normal returns),
 * so -- exactly like sysretq -- only the return address needs writing. SPSR for
 * the eret is 0: EL0t with all DAIF masks cleared (x86: RFLAGS |= IF).
 * --------------------------------------------------------------------------- */
    globl pal_syscall_return
pal_syscall_return:
    mrs     x16, tpidr_el1
    ldr     x16, [x16, #PER_CPU_DATA_CUR_TCB]
    ldr     x1, [x16, #PAL_TCB_VM_USER_RIP]
    msr     elr_el1, x1
    msr     spsr_el1, xzr
    eret

/* ---------------------------------------------------------------------------
 * void pal_sigreturn_return(void) -- x86 sigreturn_asm analog.
 *
 * rt_sigreturn must resume the *interrupted* user context recorded by LibOS in
 * thread->sigreturn_pseudo_stack (an eret-consumed frame {ELR, SPSR, SP_EL0},
 * the aarch64 replacement for the iretq frame x86 pushes here). The user x0 and
 * SP_EL0 from before rt_sigreturn clobbered them were memorized in
 * sigreturn_user_rax / sigreturn_user_rsp by LibOS's epilogue on entry; we put
 * x0 back before eret (SP_EL0 comes from the frame itself).
 * --------------------------------------------------------------------------- */
    globl pal_sigreturn_return
pal_sigreturn_return:
    mrs     x16, tpidr_el1
    ldr     x16, [x16, #PER_CPU_DATA_CUR_TCB]
    mrs     x17, sp_el0
    str     x17, [x16, #PAL_TCB_VM_SIGRETURN_USER_RSP]  /* "user rsp" memory, x86 parity */
    str     x0, [x16, #PAL_TCB_VM_SIGRETURN_USER_RAX]

    ldr     x1, [x16, #PAL_TCB_VM_SIGRETURN_PSEUDO_STACK]       /* frame.elr */
    msr     elr_el1, x1
    ldr     x1, [x16, #PAL_TCB_VM_SIGRETURN_PSEUDO_STACK + 8]   /* frame.spsr */
    msr     spsr_el1, x1
    ldr     x1, [x16, #PAL_TCB_VM_SIGRETURN_PSEUDO_STACK + 16]  /* frame.sp (user stack) */
    msr     sp_el0, x1
    ldr     x0, [x16, #PAL_TCB_VM_SIGRETURN_USER_RAX]           /* eret won't restore x0 */
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
    ldr     x3, [sp, #16]                     /* frame.sp: user SP_EL0 */
    msr     elr_el1, x1
    msr     spsr_el1, x2
    msr     sp_el0, x3

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
    /* mirror the TCB into the EL0-readable singleton (pal_vm_tcb.c) with the same address
     * arithmetic x86 gets from gsbase being ring-3-readable; no EL0 code can observe the
     * cur_tcb/singleton pair half-updated -- we are still in EL1 asm */
    adrp    x17, g_pal_tcb_singleton
    add     x17, x17, :lo12:g_pal_tcb_singleton
    str     x1, [x17]

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
 * EL1 exception entries (B2 bring-up stubs -- see the file header). The frame
 * layout they will use is struct isr_regs (kernel_interrupts.h); they must set
 * SP_EL1 from per_cpu_data->interrupt_stack first (the incoming SP_EL1 belongs
 * to whatever user stack the CPU happened to be running, see above).
 * --------------------------------------------------------------------------- */
    .balign 128
el0_irq_entry:
    b       hang

    .balign 128
el1_sync_entry:
    b       hang

    .balign 128
el1_irq_entry:
    b       hang

    .section .note.GNU-stack, "", %progbits      /* the kernel never executes the stack */
