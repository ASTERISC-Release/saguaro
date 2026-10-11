/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/*
 * riscv64 signal-context and syscall-emulation layer (port of the aarch64 libos_context.c).
 *
 * PHASE-1 STUBS: every function below aborts at runtime. The real riscv64 implementations are
 * Phase 4 of Documentation/devel/riscv/plan.md; the aarch64 sibling
 * (libos/src/arch/aarch64/libos_context.c, findings #5, #6, #15 in
 * Documentation/devel/aarch64/direct-pal-port-log.md) is the model. riscv64-relevant points the
 * real implementation must honor:
 *
 * - prepare_sigframe(): build the riscv64 rt signal frame -- all 32 GPRs (pc/ra/sp/gp/tp/... in
 *   struct riscv_user_regs field order) into sc_regs, the F/D d-state into sc_fpregs, and set the
 *   handler's return address (`ra`) to libos_rt_sigreturn_trampoline_addr() (riscv64 has no
 *   SA_RESTORER mechanism at all, same requirement as aarch64 finding #5; the kernel's own
 *   delivery pointing `ra` at the vDSO __kernel_rt_sigreturn is *not* available to us because we
 *   synthesize the frame ourselves).
 * - The frame is always exactly sizeof(struct riscv_d_ext_state) of scalar FP; the V record (if
 *   a vector-capable kernel ever adds one) must be preserved untouched -- see the V-transparency
 *   comment in pal/include/arch/riscv64/linux/ucontext.h.
 * - maybe_emulate_syscall(): seccomp-trap path; ucontext_revert_syscall() already rewinds pc by
 *   4 (riscv64 `ecall` is fixed-width).
 * - restart_syscall(): restore a0 from context->orig_a0 (a0 == arg0 == return value, the riscv64
 *   instance of aarch64's orig_x0 / x86_64's orig_rax).
 *
 * These stubs are never reached in a Phase-1 build (nothing runs the LibOS yet); the exit
 * criterion for Phase 1 is that the whole tree *compiles and links*, which these satisfy.
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

/* The tag _return_from_syscall() tests in assembly (syscallas.S spells the same constant); if
 * this ever changes, the `li` there must change with it. */
static_assert(PAL_PSTATE_EXACT_CONTEXT == (1ULL << 62),
              "syscallas.S hard-codes the exact-context tag bit");

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
     * (plan.md decisions), so there is nothing to probe for. */
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
    BUG();
}

void* libos_rt_sigreturn_trampoline_addr(void) {
    BUG();
}

void prepare_sigframe(PAL_CONTEXT* context, siginfo_t* siginfo, void* handler, void* restorer,
                      bool should_use_altstack, __sigset_t* old_mask) {
    BUG();
}

void restart_syscall(PAL_CONTEXT* context, uint64_t syscall_nr) {
    BUG();
}

void restore_sigreturn_context(PAL_CONTEXT* context, __sigset_t* new_mask) {
    BUG();
}

bool maybe_emulate_syscall(PAL_CONTEXT* context) {
    BUG();
}
