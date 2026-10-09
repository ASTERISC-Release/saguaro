/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2020 Intel Corporation
 *                    Borys Popławski <borysp@invisiblethingslab.com>
 */

/*
 * This file contains code for aarch64-specific CPU context manipulation.
 *
 * Unlike x86_64, there is no XSAVE-style variable-size "extended state" concept here (scope of
 * the aarch64 port: NEON only, no SVE/SVE2), so `struct libos_xstate` is always exactly
 * `sizeof(struct libos_fpstate)` -- no CPUID-leaf feature detection, no magic-number validation
 * scheme, no "is this an xsave-made xstate" question to ask at all.
 */

#include <stdalign.h>
#include <stdnoreturn.h>

#include "libos_context.h"
#include "libos_entry.h"
#include "libos_internal.h"
#include "libos_thread.h"
#include "pal.h"
#include "ucontext.h"

static const struct libos_xstate g_libos_xstate_reset_state
__attribute__((aligned(LIBOS_XSTATE_ALIGN))) = { 0 };

uint64_t libos_xstate_size(void) {
    return sizeof(struct libos_xstate);
}

void libos_xstate_init(void) {
    /* Nothing to detect: NEON is always present on aarch64, and the FP/SIMD state is always the
     * same fixed size. */
    log_debug("LibOS xstate_size 0x%lx(%lu)", sizeof(struct libos_xstate),
              sizeof(struct libos_xstate));
}

/* Written in asm (not relying on intrinsics/compiler-generated loads) so we have full control
 * over exactly which V registers get touched and in what order, matching x86_64's reasoning for
 * writing its XSAVE/FXSAVE restore in raw asm. */
__asm__(
".pushsection .text\n"
".global libos_xstate_restore\n"
".type libos_xstate_restore, %function\n"
"libos_xstate_restore:\n"
    /* x0: struct libos_xstate* { magic, size, fpsr, fpcr, vregs[32] } */
    "ldr  w1, [x0, #8]\n"
    "ldr  w2, [x0, #12]\n"
    "msr  fpsr, x1\n"
    "msr  fpcr, x2\n"
    "add  x0, x0, #16\n"
    "ldr  q0,  [x0], #16\n"
    "ldr  q1,  [x0], #16\n"
    "ldr  q2,  [x0], #16\n"
    "ldr  q3,  [x0], #16\n"
    "ldr  q4,  [x0], #16\n"
    "ldr  q5,  [x0], #16\n"
    "ldr  q6,  [x0], #16\n"
    "ldr  q7,  [x0], #16\n"
    "ldr  q8,  [x0], #16\n"
    "ldr  q9,  [x0], #16\n"
    "ldr  q10, [x0], #16\n"
    "ldr  q11, [x0], #16\n"
    "ldr  q12, [x0], #16\n"
    "ldr  q13, [x0], #16\n"
    "ldr  q14, [x0], #16\n"
    "ldr  q15, [x0], #16\n"
    "ldr  q16, [x0], #16\n"
    "ldr  q17, [x0], #16\n"
    "ldr  q18, [x0], #16\n"
    "ldr  q19, [x0], #16\n"
    "ldr  q20, [x0], #16\n"
    "ldr  q21, [x0], #16\n"
    "ldr  q22, [x0], #16\n"
    "ldr  q23, [x0], #16\n"
    "ldr  q24, [x0], #16\n"
    "ldr  q25, [x0], #16\n"
    "ldr  q26, [x0], #16\n"
    "ldr  q27, [x0], #16\n"
    "ldr  q28, [x0], #16\n"
    "ldr  q29, [x0], #16\n"
    "ldr  q30, [x0], #16\n"
    "ldr  q31, [x0], #16\n"
    "ret\n"
".popsection\n"
);

/* Copies FPU state. */
static void libos_xstate_copy(struct libos_xstate* dst, const struct libos_xstate* src) {
    if (src == NULL) {
        src = &g_libos_xstate_reset_state;
    }
    memcpy(dst, src, sizeof(struct libos_xstate));
}

noreturn void restore_child_context_after_clone(struct libos_context* context) {
    assert(context->regs);

    /* Set 0 as child return value (X0, not X8 -- the syscall *number* register is untouched by
     * a successful return, only the return-value register changes, same convention as any other
     * aarch64 syscall). */
    context->regs->regs[0] = 0;

    context->syscall_nr = -1;

    set_tls(context->tls);

    PAL_CONTEXT* regs = context->regs;
    context->regs = NULL;

    return_from_syscall(regs);
}

struct sigframe {
    ucontext_t uc;
    siginfo_t siginfo;
};

/* aarch64 has no SA_RESTORER-style caller-provided restorer: the arm64 kernel itself writes a
 * two-instruction trampoline (`mov x8, #139; svc #0`, i.e. `rt_sigreturn`) into the signal
 * frame and points LR at it -- verified empirically on a native arm64 box: LR inside a plain
 * handler holds the frame's address and its first words are exactly 0xd2801168/0xd4000001.
 * The arm64 glibc correspondingly does *not* inject a restorer the way the x86_64 glibc
 * sigaction() wrapper does (whose `sa_restorer` points into libc .text and which Gramine x86
 * faithfully uses) -- Gramine's aarch64 port inherited that mechanism-less design and so set
 * LR to NULL: every delivered guest handler executed `ret` to address 0. That single gap
 * accounts for the signal-cluster test failures (itimer, sigaltstack, sigprocmask_pending,
 * sighandler_sigpipe, syscall_restart test 2, ...).
 * We provide the same trampoline in libsysdb's own executable .text rather than inside the
 * frame: the arm64 kernel writes it in-frame only because its page mapping is already
 * executable for the stack; Gramine's guest stacks are non-executable and making them
 * executable per signal (or in general) would be a real security regression for zero benefit.
 * Execution happens with the application's live SP, which is all `rt_sigreturn` actually
 * consumes (same as the real kernel's trampoline: the instructions' own address is never
 * relevant, only SP is), and the `svc #0` takes the ordinary seccomp-trap -> syscall
 * emulation path -- indistinguishable from the glibc syscall wrapper's own `svc` at a libc
 * text address. */
static_assert(__NR_rt_sigreturn == 139, "hard-coded mov x8 immediate below must match");
__attribute__((naked)) static void rt_sigreturn_trampoline(void) {
    __asm__ volatile("mov x8, #139\n" /* == __NR_rt_sigreturn */
                     "svc #0\n" ::: "memory");
}

/* See libos_internal.h: whitelist hook for illegal_upcall()'s internal-execution check. */
void* libos_rt_sigreturn_trampoline_addr(void) {
    return (void*)&rt_sigreturn_trampoline;
}

void prepare_sigframe(PAL_CONTEXT* context, siginfo_t* siginfo, void* handler, void* restorer,
                      bool should_use_altstack, __sigset_t* old_mask) {
    struct libos_thread* current = get_cur_thread();

    uint64_t stack = get_stack_for_sighandler(context->sp, should_use_altstack);

    /* Unlike x86_64, there's no separate xstate scratch allocation here: aarch64's sigcontext
     * carries FP/SIMD state inline inside its own fixed __reserved[] array (already accounted
     * for in sizeof(*sigframe)) rather than through a pointer this code would need to
     * separately allocate a target for -- see pal_context_to_ucontext()'s comment. */
    struct sigframe* sigframe = NULL;
    stack = ALIGN_DOWN(stack - sizeof(*sigframe), alignof(*sigframe));
    /* AAPCS64 requires SP to be 16-byte aligned at a function's entry point (the signal handler
     * is effectively "called" here), so make sure of that explicitly -- unlike x86_64's "aligned
     * to 8 mod 0x10" rule (which accounts for a call pushing a return address onto the stack),
     * aarch64 never uses the stack for a return address at all (see the restorer/LR comment
     * below), so there's no extra offset to account for. */
    stack = ALIGN_DOWN(stack, 0x10);

    sigframe = (struct sigframe*)stack;
    memset(sigframe, 0, sizeof(*sigframe));

    sigframe->siginfo = *siginfo;

    sigframe->uc.uc_flags = 0;
    sigframe->uc.uc_link = NULL;
    /* TODO: add support for SA_AUTODISARM
     * Tracked in https://github.com/gramineproject/gramine/issues/84. */
    sigframe->uc.uc_stack = current->signal_altstack;

    /* Copies context's GPRs/FP state into sigframe->uc in one step, including planting a valid
     * fpsimd_context record inside uc_mcontext.__reserved[] (see the function's own comment). */
    pal_context_to_ucontext(&sigframe->uc, context);

    sigframe->uc.uc_sigmask = *old_mask;

    /* We always set all 3 arguments, even if it is not `SA_SIGINFO` handler. We can and it is
     * easier this way. */
    context->regs[0] = (long)siginfo->si_signo;
    context->regs[1] = (uint64_t)&sigframe->siginfo;
    context->regs[2] = (uint64_t)&sigframe->uc;

    /*
     * Unlike x86_64 (which must push `restorer` onto the stack to mimic what a `call`
     * instruction would have left there, since the kernel/our emulation jumps directly to
     * `handler` without using `call`), aarch64's calling convention already has a dedicated
     * "return to this address" register -- LR (X30) -- and the handler returns to it via a
     * plain `ret`, exactly as if it had been entered via `blr`. So we just set LR, no stack
     * space is consumed for this at all.
     *
     * The `restorer` argument (an SA_RESTORER-style caller-provided restorer, always NULL for
     * real aarch64 guests since their libc never passes one -- see
     * rt_sigreturn_trampoline()'s comment) has no meaning on this architecture: the arm64
     * kernel ignores it too. Always use our in-process trampoline instead.
     */
    __UNUSED(restorer);
    context->regs[30] = (uint64_t)&rt_sigreturn_trampoline;

    context->pc = (uint64_t)handler;
    context->sp = stack;
    /* AAPCS64 flags (NZCV in PSTATE) are caller-saved, not preserved across a function call --
     * same reasoning x86_64 uses for setting a fixed default EFLAGS here instead of carrying the
     * interrupted context's flags forward. */
    context->pstate = 0;

    log_debug("Created sigframe for sig: %d at %p (handler: %p, restorer: %p)",
              siginfo->si_signo, sigframe, handler, restorer);
}

void restart_syscall(PAL_CONTEXT* context, uint64_t syscall_nr) {
    context->regs[8] = syscall_nr;
    /* x0 carried the -ERESTART* code do_syscall() wrote as the "return value"; put the caller's
     * original first argument back before re-dispatching (arm64 kernel equivalent: restoring
     * regs[0] from pt_regs->orig_x0). Without this, e.g. a restarted `read(fd, ...)` would see
     * fd = -ERESTARTSYS. */
    context->regs[0] = context->orig_x0;
    /* Re-enter libos_syscall_entry with the resume address in X16 (the entry contract). No
     * explicit load is needed: context->regs[16] is written by libos_syscall_entry itself to
     * mirror the resume address (see syscallas.S), and neither pal_context_set_retval() nor
     * anything else in the syscall-emulation path touches regs[16] -- so _return_from_syscall()
     * restoring X16 from regs[16] here re-establishes the contract for the re-entry. context->pc
     * is overwritten below, which is exactly what the entry expects (pc = entry point, X16 =
     * where the restarted svc will return to). */
    context->pc = (uint64_t)&libos_syscall_entry;
}

/* `struct libos_xstate` and `PAL_XREGS_STATE` are the same structure under different names (one
 * declared for LibOS's own use, one for PAL's), same as x86_64 -- see the static_assert below. */
static_assert(sizeof(struct libos_xstate) == sizeof(PAL_XREGS_STATE),
              "FP/SIMD state structs differ");

void restore_sigreturn_context(PAL_CONTEXT* context, __sigset_t* new_mask) {
    struct sigframe* sigframe = (struct sigframe*)context->sp;
    *new_mask = sigframe->uc.uc_sigmask;

    struct libos_xstate* syscall_fpregs_buf = (struct libos_xstate*)context->fpregs;
    assert(syscall_fpregs_buf);

    ucontext_to_pal_context(context, &sigframe->uc);
    /* ucontext_to_pal_context() just pointed context->fpregs at the fpsimd_context record that
     * lives inside sigframe's own memory, which is about to be unwound/overwritten once this
     * signal frame is torn down -- copy it into the syscall-entry-reserved scratch buffer (saved
     * above, before the call) that will actually outlive this function, matching what x86_64
     * does for the same reason. */
    libos_xstate_copy(syscall_fpregs_buf, (struct libos_xstate*)context->fpregs);
    context->fpregs = (PAL_XREGS_STATE*)syscall_fpregs_buf;
    context->is_fpregs_used = 1;
}

bool maybe_emulate_syscall(PAL_CONTEXT* context) {
    uint8_t* pc = (uint8_t*)context->pc;
    /* `svc #0` encodes as the little-endian byte sequence 01 00 00 d4; every aarch64 instruction
     * is 4 bytes, unlike x86_64's variable-length `syscall` (2 bytes: 0f 05). */
    bool is_svc = pc[0] == 0x01 && pc[1] == 0x00 && pc[2] == 0x00 && pc[3] == 0xd4;
    /* If another thread rewrote this very site between our trap and this check, the word is now a
     * branch to one of our stubs; the trap is still a legitimate syscall. */
    if (is_svc || libos_syscall_site_was_patched((uintptr_t)pc)) {
        /* Put the resume address ("the instruction after the trapped svc") in X16 -- the entry
         * contract's designated carrier, matching the GRAMINE_SYSCALL path (`adr x16, 1f`) and
         * regs[16]'s new role as the resume mirror (see syscallas.S).
         *
         * This must NOT be regs[30]: x86_64 can stash the resume address in RCX because the real
         * `syscall` instruction itself destroys RCX, so no application holds live state there at
         * a syscall site. aarch64's `svc` destroys nothing -- regs[30] at this point is the
         * application's *live* link register (leaf libc syscall wrappers `ret` via it
         * immediately), and overwriting it corrupts the caller's return path into an infinite
         * self-loop. That clobber was the root cause of the aarch64 helloworld hang; see
         * Documentation/devel/aarch64/direct-pal-port-log.md, finding #1. regs[30] is instead saved
         * and restored untouched as application state
         * (ucontext_to_pal_context()/pal_context_to_ucontext() already do this). */
        context->regs[16] = (uint64_t)pc + 4;
        context->pc = (uint64_t)&libos_syscall_entry;
        /* Prototype fast path (opt-in): rewrite this site so it stops trapping. The current
         * trap is still serviced through the emulation entry set up above. */
        if (is_svc)
            libos_try_patch_syscall_site((uintptr_t)pc);
        return true;
    }
    return false;
}
