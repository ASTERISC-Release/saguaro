/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2023 Intel Corporation */

/*
 * Declarations for threads and their contexts (for switching).
 *
 * Also declares idle and bottomhalves thread loops.
 */

#pragma once

#include <stdint.h>

#include "api.h"
#include "list.h"

#include "kernel_sched.h"

#define THREAD_STACK_SIZE (PAGE_SIZE * 16) /* 64KB user stack */
#define ALT_STACK_SIZE    (PAGE_SIZE * 2)  /* 8KB signal stack */

enum thread_state {
    THREAD_STOPPED,
    THREAD_RUNNABLE,
    THREAD_BLOCKED,
    THREAD_RUNNING,
};

#if defined(__x86_64__) || defined(__aarch64__)
struct memory_address_space;
#endif

/*
 * Kernel-mode switch context. On x86_64 this is the explicit register set saved by
 * save_context_and_restore_next (kernel_events.S). On aarch64 the same struct doubles as the
 * frame built by the EL0 sync (timer-IRQ preemption and svc entry) handlers, so it mirrors the
 * aarch64 PAL_CONTEXT layout (pal/include/arch/aarch64/pal_arch.h) field-for-field at the same
 * offsets -- pal_common_exception.c's field-copy helpers rely on that -- plus the two
 * kernel-only fields the asm needs (the `svc` resume address and the user TLS register value,
 * the TPIDR_EL0 analogs of x86_64's user_rip/user_fsbase).
 */
struct thread_context {
#ifdef __x86_64__
    uint64_t r8;
    uint64_t r9;
    uint64_t r10;
    uint64_t r11;
    uint64_t r12;
    uint64_t r13;
    uint64_t r14;
    uint64_t r15;
    uint64_t rdi;
    uint64_t rsi;
    uint64_t rbp;
    uint64_t rbx;
    uint64_t rdx;
    uint64_t rax;
    uint64_t rcx;
    uint64_t rsp;
    uint64_t rip;
    uint64_t rflags;

	uint64_t user_rip;
	uint64_t user_fsbase;

    uint64_t csgsfsss;
    uint64_t err;
    uint64_t trapno;
    uint64_t oldmask;
    uint64_t cr2;

    void* fpregs; /* XSAVE area */
#else /* __aarch64__ */
    uint64_t regs[31];        /* x0..x30 */
    uint64_t sp;              /* SP_EL0 (user frame) / kernel SP (PAL-level switch) */
    uint64_t pc;              /* ELR_EL1 at frame time */
    uint64_t pstate;          /* SPSR_EL1 at frame time; DAIF-masked-IRQ state for PAL switches */
    uint64_t fault_address;   /* FAR_EL1 */
    uint64_t esr;             /* ESR_EL1 */
    uint64_t oldmask;

    void* fpregs;             /* FP/SIMD save area (PAL_XREGS_STATE layout) */
    uint32_t fpsr;
    uint32_t fpcr;
    uint8_t is_fpregs_used;
    uint8_t _pad[3];
    uint64_t orig_x0;

    uint64_t user_rip;        /* ELR_EL1 saved at `svc` entry: address to resume user at */
    uint64_t user_tls;        /* TPIDR_EL0 value belonging to this thread (x86 user_fsbase) */
    /* Syscall snapshot of the EL0 registers that the Linux syscall ABI hands back intact:
     * x1..x15 (slots 0..14), x18 (slot 15) and x17 (slot 16). x0 is the result slot and x16
     * is the kernel's own scratch -- the analog of x86's syscall-instruction rcx/r11 clobber,
     * which glibc declares dead for exactly that reason. Everything x0..x18 is *C*-ABI
     * caller-saved and the LibOS dispatcher is C code running at EL1, so without this frame
     * the app's live values die in the dispatcher: Linux takes a full pt_regs snapshot per
     * svc and hides this. First two victims without it, both from glibc static-pie malloc
     * init: ptmalloc_init poisons the heap as `mov x1,#8; svc (getrandom); add x19, sp, x1`
     * (reusing the preserved length argument), and the tunables machinery reads
     * `ldr x8, [x18, #200]`-style slots with x18 pinned to tunable_list across syscalls. The
     * clone3 child's `blr x11` contract (fn/arg inherited across the parent's svc) is what
     * caught the original off-by-one that dropped x9 and shifted x10..x15 down a slot. */
    uint64_t syscall_scratch[18];
#endif
}
/* On x86_64 all members are 8-byte naturally-aligned, so packed is a no-op kept for upstream
 * layout-compat reasons. On aarch64 it would misalign `orig_x0`/`user_rip`/`user_tls` (4-byte
 * slots), and unaligned uint64 offsets are not even encodable in ldr/str — so the aarch64
 * variant uses natural alignment; `_pad` keeps the frame a multiple of 16 bytes. */
#ifdef __x86_64__
__attribute__((packed))
#endif
;

/*
 * "Pseudo stack" for returning to EL0 with an explicit exception-return frame. On x86 this is
 * literally the stack frame `iretq` pops (RIP/CS/RFLAGS/RSP/SS); on aarch64 the same role is
 * played by the three system registers an `eret` consumes: ELR_EL1 (return address), SPSR_EL1
 * (saved PSTATE incl. condition/IRQ masks) and SP_EL0 (the user stack pointer, which `eret`
 * does *not* restore automatically -- the handler writes it right before eret).
 */
struct thread_irq_pseudo_stack {
#ifdef __x86_64__
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t rsp;
    uint64_t ss;
#else /* __aarch64__ */
    uint64_t elr;    /* ELR_EL1: user return address (x86 rip) */
    uint64_t spsr;   /* SPSR_EL1: saved user PSTATE (x86 rflags; cs/ss have no analog) */
    uint64_t sp;     /* SP_EL0: user stack pointer (x86 rsp) */
#endif
}
#ifdef __aarch64__
/* isr_eret_to_userland points SP_EL1 at this struct and reads it with ldp [sp]: with
 * SP alignment checking on, a frame at an address that is 8 (mod 16) takes an
 * SP-alignment fault, and which side of 16 it lands on depends on the layout of
 * struct thread. (Size 24 pads to 32; asm only touches offsets 0..23.) */
__attribute__((packed, aligned(16)))
#else
__attribute__((packed))
#endif
;

DEFINE_LIST(thread);
struct thread {
    LIST_TYPE(thread) list;
    enum thread_state state;
    uint32_t thread_id; /* for debugging purposes */
    int* blocked_on;
    bool is_helper; /* is it an idle or bottomhalves thread */

#if defined(__x86_64__) || defined(__aarch64__)
    struct memory_address_space* address_space;
    /* set by _PalThreadResume(): deliver PAL_EVENT_INTERRUPTED when the thread next runs in user
     * mode (see pal_common_deliver_pending_interrupt()) */
    bool resume_pending;
    /* set when the timer found the thread in kernel mode (multiprocess support only): its next
     * return to user mode takes the tick instead, see sysret_asm in kernel_events.S */
    bool preempt_pending;
#endif

    /* CPU affinity of a thread */
    unsigned long cpu_mask[MAX_NUM_CPU_LONGS];

    /* for context switching: GPRs + XSAVE area of a thread during kernel execution (when it
     * explicitly yields, i.e., we implement cooperative kernel scheduling) or during userland
     * execution (when it is interrupted by IRQ); XSAVE is always stored in a preallocated region */
    struct thread_context context;

#ifdef __aarch64__
    /* isr_eret_to_userland runs with SP_EL1 = &irq_pseudo_stack and IRQs already unmasked by the
     * context switch. aarch64 has no IST: el1_irq_entry carves its 48-byte scratch *below the
     * incoming SP*, so an IRQ (shootdown SGI, virtio) landing in that window would write over the
     * 48 bytes preceding this field -- i.e. the tail of `context` (pc/sp/pstate/user_tls/
     * fpregs): garbage callback, wrong sp, corrupted registers on the thread's next dispatch.
     * Keep dead space under the pseudo stack for that scratch. */
    uint8_t irq_pseudo_stack_scratch[64] __attribute__((aligned(16)));
#endif
    struct thread_irq_pseudo_stack irq_pseudo_stack;

    /* per-thread scratch registers for returning to userspace from rt_sigreturn() syscall;
     * sigreturn flow is different from normal-return-to-userspace sysret flow in that sigreturn
     * uses iretq instruction which requires RIP,RSP,RFLAGS to be located on a (pseudo) stack;
     * see also kernel_events.S */
    uint64_t sigreturn_user_rsp;
    uint64_t sigreturn_user_rax;
    uint64_t sigreturn_pseudo_rsp; /* &sigreturn_pseudo_stack + sizeof(thread_irq_pseudo_stack) */
    struct thread_irq_pseudo_stack sigreturn_pseudo_stack;

#ifdef __x86_64__
    /* a LibOS upcall for an event of this thread in user mode, run outside the interrupt
     * handler (see pal_common_exception.c) */
    struct isr_regs upcall_regs;
    void* upcall_fpregs;
    uint64_t upcall_addr;
    int upcall_event;

    /* The application's own GS base (arch_prctl(ARCH_SET_GS), Wine's TEB), 0 when it never set
     * one. While it is set, user code runs with it in GS and every entry into the kernel swaps the
     * PAL TCB back in (kernel_events.S: syscall_asm_gs, _isr_common); every return to user mode
     * loads it again. Threads with 0 run the old paths unchanged. (Last in the thread, which is
     * last in the TCB: the offsets of every other field stay as they were.) */
    uint64_t user_gsbase;
    /* the interrupted context ran with the application's base (isr_regs.gs_swapped) and returns
     * through isr_iret_to_userland (a preempted thread, a deferred upcall), which loads the base
     * back and clears this */
    uint64_t gs_swapped_by_isr;
#endif

    /* An interruptible wait (sched_thread_wait_interruptible(): a blocking PAL call made for the
     * application) is in progress; _PalThreadResume() of another thread then wakes this one with
     * `woken_by_interrupt` set and the wait returns -PAL_ERROR_INTERRUPTED, as a host signal
     * interrupts a system call on the Linux PAL. Both under g_thread_list_lock. */
    bool wait_interruptible;
    bool woken_by_interrupt;
};
DEFINE_LISTP(thread);

int thread_get_stack_and_fpregs(void** out_stack, void** out_fpregs);
noreturn void thread_free_stack_and_die(void* thread_stack, int* clear_child_tid);
/* Returns the stack of a thread that will never run again (killed with its process) for reuse. */
void thread_release_stack(void* thread_stack);

void thread_setup(struct thread* thread, void* fpregs, void* stack, int (*callback)(void*),
                  const void* param);
int thread_helper_create(int (*callback)(void*), struct thread** out_thread);

noreturn int thread_idle_run(void* args);
noreturn int thread_bottomhalves_run(void* args);
