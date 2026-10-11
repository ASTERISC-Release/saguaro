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

#include <stdnoreturn.h>

#include "libos_context.h"
#include "libos_internal.h"
#include "libos_tcb.h"
#include "pal.h"

uint64_t libos_xstate_size(void) {
    BUG();
}

void libos_xstate_init(void) {
    BUG();
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
