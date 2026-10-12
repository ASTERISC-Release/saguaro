/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/*
 * riscv64 signal-context and syscall-emulation layer (port of the aarch64 libos_context.c).
 *
 * The aarch64 sibling (libos/src/arch/aarch64/libos_context.c) is the model. Two riscv64
 * differences drive most of what follows: `tp` is a general-purpose register (so it is part of
 * the saved context), and a syscall must give back every register except a0 (see syscallas.S).
 */

#include <stdalign.h>
#include <stdnoreturn.h>
#include <string.h>

#include "libos_context.h"
#include "libos_entry.h"
#include "libos_internal.h"
#include "libos_tcb.h"
#include "libos_thread.h"
#include "pal.h"
#include "ucontext.h"

static_assert(offsetof(PAL_TCB, libos_tcb) + offsetof(libos_tcb_t, syscall_scratch_pc)
                  == GRAMINE_SYSCALL_RESUME_OFFSET,
              "GRAMINE_SYSCALL_RESUME_OFFSET must match");

/* `struct libos_xstate` and `PAL_XREGS_STATE` are the same structure under two names (one for
 * LibOS, one for PAL), as on x86_64/aarch64 -- and on riscv64 both are exactly the kernel's
 * scalar F/D state (32 64-bit f registers + fcsr), no record headers, no variable size. */
static_assert(sizeof(struct libos_xstate) == sizeof(PAL_XREGS_STATE),
              "FP state structs differ");

static const struct libos_xstate g_libos_xstate_reset_state
__attribute__((aligned(LIBOS_XSTATE_ALIGN))) = { 0 };

uint64_t libos_xstate_size(void) {
    return sizeof(struct libos_xstate);
}

void libos_xstate_init(void) {
    /* Nothing to detect: the lp64d ABI implies F/D always (a hard requirement, not a runtime
     * feature), and the state is always the same fixed size. The vector record is out of scope
     * (PLAN.md decisions), so there is nothing to probe for. */
    log_debug("LibOS xstate_size 0x%lx(%lu)", sizeof(struct libos_xstate),
              sizeof(struct libos_xstate));
}

/* Written in asm (not relying on compiler-generated loads) so we control exactly which f
 * registers get touched and in what order, matching x86_64's (XSAVE) and aarch64's (ld q0..q31)
 * reasoning. The control word is installed first: loading f registers cannot raise an FP
 * exception, so the restored FFLAGS bits are never perturbed afterwards. */
__asm__(
".pushsection .text\n"
".global libos_xstate_restore\n"
".type libos_xstate_restore, @function\n"
"libos_xstate_restore:\n"
    /* a0: struct libos_xstate* { f[32], fcsr } -- the kernel's __riscv_d_ext_state layout */
    "lw   t0, 256(a0)\n"
    "csrw fcsr, t0\n"
    "fld  f0,  0(a0)\n"
    "fld  f1,  8(a0)\n"
    "fld  f2, 16(a0)\n"
    "fld  f3, 24(a0)\n"
    "fld  f4, 32(a0)\n"
    "fld  f5, 40(a0)\n"
    "fld  f6, 48(a0)\n"
    "fld  f7, 56(a0)\n"
    "fld  f8, 64(a0)\n"
    "fld  f9, 72(a0)\n"
    "fld f10, 80(a0)\n"
    "fld f11, 88(a0)\n"
    "fld f12, 96(a0)\n"
    "fld f13,104(a0)\n"
    "fld f14,112(a0)\n"
    "fld f15,120(a0)\n"
    "fld f16,128(a0)\n"
    "fld f17,136(a0)\n"
    "fld f18,144(a0)\n"
    "fld f19,152(a0)\n"
    "fld f20,160(a0)\n"
    "fld f21,168(a0)\n"
    "fld f22,176(a0)\n"
    "fld f23,184(a0)\n"
    "fld f24,192(a0)\n"
    "fld f25,200(a0)\n"
    "fld f26,208(a0)\n"
    "fld f27,216(a0)\n"
    "fld f28,224(a0)\n"
    "fld f29,232(a0)\n"
    "fld f30,240(a0)\n"
    "fld f31,248(a0)\n"
    "ret\n"
".popsection\n"
);

/* Copies FPU state. NULL means "the all-zero reset state" (the x86_64 FXSAVE-area convention). */
__attribute__((unused)) /* used by restore_sigreturn_context(), implemented further below */
static void libos_xstate_copy(struct libos_xstate* dst, const struct libos_xstate* src) {
    if (src == NULL) {
        src = &g_libos_xstate_reset_state;
    }
    memcpy(dst, src, sizeof(struct libos_xstate));
}

noreturn void restore_child_context_after_clone(struct libos_context* context) {
    assert(context->regs);

    /* 0 is the child's return value: `a0` (not `a7`, the syscall-number register -- a successful
     * clone returns through the ordinary result register, like any riscv64 syscall). */
    context->regs->regs[10] = 0;

    context->syscall_nr = -1;

    /* The application's TLS pointer is the `tp` GPR on riscv64 (no wrmsr/MSR dance as on x86_64,
     * no special register as on aarch64), and installing it is also what registers this thread
     * in the per-thread PAL_TCB registry -- see set_tls()'s comment in libos_tcb_arch.h. It must
     * happen before return_from_syscall(), whose entry code may resolve pal_get_tcb(). */
    set_tls(context->tls);

    /* `tp` is a GPR on riscv64, so the saved context carries it too: regs[4] is a copy of the
     * *parent's* TLS pointer (the context was copied at the parent's syscall entry), and
     * _return_from_syscall() reloads `tp` from it, undoing the set_tls() above. The child would
     * then run on the parent's TLS and, through the PAL_TCB registry, on the parent's LibOS stack.
     * context->tls is the right value in every case (without CLONE_SETTLS it is the parent's own
     * pointer). x86_64 and aarch64 keep the TLS pointer outside the saved registers. */
    context->regs->regs[4] = context->tls;

    PAL_CONTEXT* regs = context->regs;
    context->regs = NULL;

    return_from_syscall(regs);
}

/*
 * The handler-return trampoline.
 *
 * riscv64 has nothing resembling x86_64's SA_RESTORER (its `struct sigaction` has no usable
 * restorer mechanism at all -- see the comment in pal/include/arch/riscv64/linux/ucontext.h and
 * the `#ifndef SA_RESTORER` call sites in libos_signal.c), and unlike the riscv64 *kernel*, which
 * points `ra` at the vDSO's `__kernel_rt_sigreturn` when delivering a signal natively, Gramine
 * synthesizes the frame itself and must therefore install the return address itself -- aarch64
 * finding #5 verbatim, down to the symptom (without this, every handler's `ret` goes to whatever
 * `ra` the interrupted context happened to hold).
 *
 * As on aarch64 it lives in libsysdb's own executable .text rather than inside the frame: the
 * kernel can put its trampoline in the frame only because its stack pages are executable, and
 * making Gramine's guest stacks executable to enable a signal restorer would be a real security
 * regression for no benefit. Execution happens with the application's live SP -- all `rt_sigreturn`
 * consumes -- and the `ecall` below is deliberately an ordinary one: the seccomp filter traps it
 * exactly like the guest libc's own syscall `ecall`s, so it is emulated as the `rt_sigreturn`
 * syscall through the standard path (maybe_emulate_syscall() -> libos_syscall_entry()), with the
 * resume address being the instruction after it. Nothing arch-specific to get wrong there: `ecall`
 * has no compressed encoding (Phase 0), so its whitelist check is exact.
 */
static_assert(__NR_rt_sigreturn == 139, "hard-coded `li a7` immediate below must match");
__asm__(
".pushsection .text\n"
".balign 4\n"
".global libos_rt_sigreturn_trampoline\n"
".type libos_rt_sigreturn_trampoline, @function\n"
"libos_rt_sigreturn_trampoline:\n"
    "li   a7, 139\n" /* == __NR_rt_sigreturn */
    "ecall\n"
    ".size libos_rt_sigreturn_trampoline, .-libos_rt_sigreturn_trampoline\n"
".popsection\n"
);
extern void libos_rt_sigreturn_trampoline(void);

/* See libos_internal_arch.h: whitelist hook for illegal_upcall()'s internal-execution check. */
void* libos_rt_sigreturn_trampoline_addr(void) {
    return (void*)&libos_rt_sigreturn_trampoline;
}

struct sigframe {
    ucontext_t uc;
    siginfo_t siginfo;
};

void prepare_sigframe(PAL_CONTEXT* context, siginfo_t* siginfo, void* handler, void* restorer,
                      bool should_use_altstack, __sigset_t* old_mask) {
    struct libos_thread* current = get_cur_thread();

    uintptr_t stack = get_stack_for_sighandler(pal_context_get_sp(context), should_use_altstack);

    /* No separate xstate scratch allocation, as on aarch64: the riscv64 sigcontext carries the
     * F/D state *inline* in sc_fpregs (already part of sizeof(*sigframe)), not through a pointer
     * this code would have to allocate a target for -- see pal_context_to_ucontext(). (The
     * kernel's optional vector record would live past the frame; vector state is out of scope,
     * and the frame we write never reaches it.) */
    struct sigframe* sigframe = NULL;
    stack = ALIGN_DOWN(stack - sizeof(*sigframe), alignof(*sigframe));
    /* The riscv64 psABI requires SP to be 16-byte aligned at any ABI call boundary (a signal
     * handler is entered like a call here). Unlike x86_64's "8 mod 16" rule there is no pushed
     * return address to account for: the return address travels in `ra` (see below), exactly as
     * on aarch64. */
    stack = ALIGN_DOWN(stack, 0x10);

    sigframe = (struct sigframe*)stack;
    memset(sigframe, 0, sizeof(*sigframe));

    sigframe->siginfo = *siginfo;

    sigframe->uc.uc_flags = 0;
    sigframe->uc.uc_link = NULL;
    /* TODO: add support for SA_AUTODISARM
     * Tracked in https://github.com/gramineproject/gramine/issues/84. */
    sigframe->uc.uc_stack = current->signal_altstack;

    /* GPRs, pc and FP state into the frame in one step (pal_context_to_ucontext() physically
     * copies the F/D registers; the write is confined to the 264-byte d-state). */
    pal_context_to_ucontext(&sigframe->uc, context);

    sigframe->uc.uc_sigmask = *old_mask;

    /* Always all 3 arguments, even for a non-SA_SIGINFO handler (allowed and simpler), in
     * riscv64's argument registers a0/a1/a2. */
    context->regs[10] = (long)siginfo->si_signo;
    context->regs[11] = (uint64_t)&sigframe->siginfo;
    context->regs[12] = (uint64_t)&sigframe->uc;

    /*
     * The handler's return address goes in `ra` -- riscv64's calling convention has a dedicated
     * "return to this address" register just like aarch64's LR, so no stack space is consumed
     * for it and the handler's plain `ret` lands here, exactly as if it had been entered with
     * `jal`.
     *
     * The `restorer` argument has no meaning on this architecture (there is no SA_RESTORER flag
     * to produce one, so callers pass NULL; libos_signal.c guards the only path that could have
     * passed something else). Always use our own trampoline.
     */
    __UNUSED(restorer);
    context->regs[1] = (uint64_t)&libos_rt_sigreturn_trampoline;

    context->pc = (uint64_t)handler;
    pal_context_set_sp(context, stack);
    /* Clearing pstate is load-bearing, not cosmetic: it drops PAL_PSTATE_EXACT_CONTEXT that
     * ucontext_to_pal_context() may have tagged this context with, and riscv64 has no condition
     * flags to carry over anyway. Entering the handler is a fresh call, not an exact restore --
     * with the tag left set, _return_from_syscall() would ebreak back into the *interrupted*
     * context instead of jumping to the handler. */
    context->pstate = 0;

    log_debug("Created sigframe for sig: %d at %p (handler: %p, restorer: %p)",
              siginfo->si_signo, sigframe, handler, restorer);
}

void restart_syscall(PAL_CONTEXT* context, uint64_t syscall_nr) {
    context->regs[17] = syscall_nr;
    /* a0 carried the -ERESTART* code do_syscall() wrote as the "return value"; put the caller's
     * original first argument back before re-dispatching (a0 is both arg0 and the result register
     * on riscv64; the riscv64 kernel keeps the same distinction through pt_regs, and Gramine
     * reproduces it with orig_a0 -- aarch64 finding #6 verbatim). Without this, a restarted
     * `read(fd, ...)` would see fd = -ERESTARTSYS. */
    context->regs[10] = context->orig_a0;
    /* Restart the way the kernel does: step back onto the application's own `ecall`, which
     * traps again and comes back through maybe_emulate_syscall(). Jumping to
     * libos_syscall_entry() directly is not possible: its resume address lives in the TCB, and
     * this context may first be parked in a signal frame while a handler runs and issues
     * syscalls of its own. Every emulated syscall on riscv64 comes from a 4-byte `ecall` (no
     * syscall site is patched), so pc - 4 is exact. */
    context->pc -= 4;
}

void restore_sigreturn_context(PAL_CONTEXT* context, __sigset_t* new_mask) {
    struct sigframe* sigframe = (struct sigframe*)pal_context_get_sp(context);
    *new_mask = sigframe->uc.uc_sigmask;

    /* The scratch buffer libos_syscall_entry() reserved below the context for context->fpregs
     * (see syscallas.S), saved before the conversion below re-points fpregs at the frame. */
    struct libos_xstate* syscall_fpregs_buf = (struct libos_xstate*)context->fpregs;
    assert(syscall_fpregs_buf);

    ucontext_to_pal_context(context, &sigframe->uc);
    /* ucontext_to_pal_context() just pointed context->fpregs at the F/D block *inside* the signal
     * frame, which is application stack memory about to be unwound/overwritten as soon as this
     * returns -- copy into the syscall-entry-reserved buffer that outlives this function, exactly
     * what x86_64/aarch64 do for the same reason. */
    libos_xstate_copy(syscall_fpregs_buf, (struct libos_xstate*)context->fpregs);
    context->fpregs = (PAL_XREGS_STATE*)syscall_fpregs_buf;
    context->is_fpregs_used = 1;
}

bool maybe_emulate_syscall(PAL_CONTEXT* context) {
    uint8_t* pc = (uint8_t*)context->pc;
    /* `ecall` encodes as the little-endian byte sequence 73 00 00 00. riscv64 instructions are
     * 4 bytes and `ecall` has *no* compressed encoding (unlike `ebreak`, which the toolchain
     * happily emits as 2-byte `c.ebreak` -- Phase 3 log), so this test is exact with no
     * halfword special-casing.
     *
     * `pc` points *at* the ecall because the PAL already applied ucontext_revert_syscall() (the
     * shared SIGSYS handler in pal_exception.c rewinds the si_call_addr, which points past the
     * instruction, by 4 before the context ever reaches us). */
    bool is_ecall = pc[0] == 0x73 && pc[1] == 0x00 && pc[2] == 0x00 && pc[3] == 0x00;
    if (is_ecall || libos_syscall_site_was_patched((uintptr_t)pc)) {
        /* Leave the resume address ("the instruction after the trapped ecall") in the TCB for
         * libos_syscall_entry() and touch no register: the kernel's `ecall` preserves everything
         * but a0, glibc's inline syscalls rely on that (even for t0-t6), and `ra` in particular
         * is the application's live link register here. The syscall number is already where it
         * belongs (a7 = regs[17]).
         *
         * A racing handler can instead see the site's published `jal`; the architecture hook
         * recognizes that case and follows the same emulation path. */
        LIBOS_TCB_SET(syscall_scratch_pc, (void*)(pc + 4));
        context->pc = (uint64_t)&libos_syscall_entry;
        if (is_ecall)
            libos_try_patch_syscall_site((uintptr_t)pc);
        return true;
    }
    return false;
}
