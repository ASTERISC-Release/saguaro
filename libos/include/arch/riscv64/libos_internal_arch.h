/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

#pragma once

#define LIBOS_ELF_HOST_MACHINE EM_RISCV

#include <stdbool.h>
#include <stdint.h>

#include "pal.h"

/* Address of the `li a7, #__NR_rt_sigreturn; ecall` restorer trampoline that prepare_sigframe()
 * installs as the handler's return address (ra). riscv64 has no glibc-provided SA_RESTORER to
 * point at -- there is not even an sa_restorer *field* in this ABI's struct sigaction (see the
 * file comment in arch/riscv64/pal_arch_exception.c) -- so, exactly like aarch64 finding #5, the
 * LibOS must supply its own return trampoline (see rt_sigreturn_trampoline()'s comment in
 * arch/riscv64/libos_context.c). Note the riscv64 kernel is itself expected to point `ra` at the
 * vDSO's __kernel_rt_sigreturn when delivering signals natively (the arm64-kernel behavior this
 * mirrors) -- to be verified against the running guest (PLAN.md "still to verify"). */
void* libos_rt_sigreturn_trampoline_addr(void);

/* SIGILL handling hooks used by illegal_upcall() (see bookkeep/libos_signal.c). */

/* True if an illegal instruction at \p addr is an intentional Saguaro-internal one: here, the
 * `ecall` inside the rt_sigreturn trampoline. Anything else in Saguaro-internal memory taking an
 * illegal instruction is a Saguaro bug. (The exact-context-restore `ebreak` of finding #15's riscv
 * counterpart is a *trap*, not an illegal instruction, and is consumed by the PAL's SIGTRAP
 * handler before ever reaching this path.) */
bool libos_arch_is_intentional_internal_insn(uintptr_t addr);

/* Give the architecture a chance to repair the SIGILL in \p context instead of delivering it to
 * the application. riscv64 has no counterpart to aarch64's stale pointer-authentication
 * signatures (no PAC in this ISA), so this always returns false -- kept as a hook so the shared
 * illegal_upcall() code stays arch-neutral. */
bool libos_arch_repair_illegal_insn(PAL_CONTEXT* context);

/* Lazy rewriting of trapped `ecall` sites (sys.rewrite_syscalls); see
 * arch/riscv64/libos_syscall_patch.c. The first tries to rewrite the site at \p pc; the second
 * recognizes a site that another thread rewrote between this thread's trap and its handler.
 * Deliberately never enabled on riscv64 for now (dev docs: riscv/PLAN.md decisions:
 * seccomp-trap only, no syscall-site rewriting -- a far jump needs 8 bytes over an `ecall`'s 4). */
bool libos_try_patch_syscall_site(uintptr_t pc);
bool libos_syscall_site_was_patched(uintptr_t pc);

/* riscv64 restart mirror: a0 is both the first syscall argument and the return-value register
 * (same shape as aarch64's x0), so the argument is saved before the return value overwrites it
 * and restored when the syscall is restarted (the same trick the riscv *kernel* would need were
 * it not keeping the full GPR set in the sigcontext; aarch64 finding #6 / pt_regs->orig_x0).
 * x86_64's return register (%rax) is distinct from its first argument register (%rdi), so it
 * needs nothing. */
static inline void libos_arch_save_restart_arg0(PAL_CONTEXT* context) {
    context->orig_a0 = context->regs[10];
}
