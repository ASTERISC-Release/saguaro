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
} __attribute__((packed));

DEFINE_LIST(thread);
struct thread {
    LIST_TYPE(thread) list;
    enum thread_state state;
    uint32_t thread_id; /* for debugging purposes */
    int* blocked_on;
    bool is_helper; /* is it an idle or bottomhalves thread */

    /* CPU affinity of a thread */
    unsigned long cpu_mask[MAX_NUM_CPU_LONGS];

    /* for context switching: GPRs + XSAVE area of a thread during kernel execution (when it
     * explicitly yields, i.e., we implement cooperative kernel scheduling) or during userland
     * execution (when it is interrupted by IRQ); XSAVE is always stored in a preallocated region */
    struct thread_context context;

    struct thread_irq_pseudo_stack irq_pseudo_stack;

    /* per-thread scratch registers for returning to userspace from rt_sigreturn() syscall;
     * sigreturn flow is different from normal-return-to-userspace sysret flow in that sigreturn
     * uses iretq instruction which requires RIP,RSP,RFLAGS to be located on a (pseudo) stack;
     * see also kernel_events.S */
    uint64_t sigreturn_user_rsp;
    uint64_t sigreturn_user_rax;
    uint64_t sigreturn_pseudo_rsp; /* &sigreturn_pseudo_stack + sizeof(thread_irq_pseudo_stack) */
    struct thread_irq_pseudo_stack sigreturn_pseudo_stack;
};
DEFINE_LISTP(thread);

int thread_get_stack_and_fpregs(void** out_stack, void** out_fpregs);
noreturn void thread_free_stack_and_die(void* thread_stack, int* clear_child_tid);

void thread_setup(struct thread* thread, void* fpregs, void* stack, int (*callback)(void*),
                  const void* param);
int thread_helper_create(int (*callback)(void*), struct thread** out_thread);

noreturn int thread_idle_run(void* args);
noreturn int thread_bottomhalves_run(void* args);
