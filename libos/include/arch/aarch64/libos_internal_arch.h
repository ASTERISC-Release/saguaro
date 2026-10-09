/* SPDX-License-Identifier: LGPL-3.0-or-later */

#pragma once

#define LIBOS_ELF_HOST_MACHINE EM_AARCH64

#include <stdbool.h>
#include <stdint.h>

#include "pal.h"

/* Address of the two-instruction `mov x8, #__NR_rt_sigreturn; svc` restorer trampoline that
 * prepare_sigframe() installs as the handler's return address (LR). aarch64 has no glibc-provided
 * SA_RESTORER to point at (unlike x86_64, whose libc .text restorer is indistinguishable from
 * guest code to illegal_upcall()'s internal-execution check). See rt_sigreturn_trampoline()'s
 * comment in arch/aarch64/libos_context.c. */
void* libos_rt_sigreturn_trampoline_addr(void);

/* SIGILL handling hooks used by illegal_upcall() (see bookkeep/libos_signal.c). */

/* True if an illegal instruction at \p addr is an intentional Gramine-internal one: here, the
 * `svc` inside the rt_sigreturn trampoline. Anything else in Gramine-internal memory taking an
 * illegal instruction is a Gramine bug. */
bool libos_arch_is_intentional_internal_insn(uintptr_t addr);

/* Give the architecture a chance to repair the SIGILL in \p context instead of delivering it to
 * the application (here: stale pointer-authentication signatures in forked children). Returns
 * true if the context was repaired and execution should simply resume. */
bool libos_arch_repair_illegal_insn(PAL_CONTEXT* context);

/* Lazy rewriting of trapped `svc #0` sites (sys.rewrite_syscalls); see
 * arch/aarch64/libos_syscall_patch.c. The first tries to rewrite the site at \p pc; the second
 * recognizes a site that another thread rewrote between this thread's trap and its handler. */
bool libos_try_patch_syscall_site(uintptr_t pc);
bool libos_syscall_site_was_patched(uintptr_t pc);

/* aarch64 restart mirror: x0 is both the first syscall argument and the return-value register, so
 * the argument is saved before the return value overwrites it and restored when the syscall is
 * restarted (mirroring the arm64 kernel's pt_regs->orig_x0). See restart_syscall(). x86_64's
 * return register (%rax) is distinct from its first argument register (%rdi), so it needs
 * nothing. */
static inline void libos_arch_save_restart_arg0(PAL_CONTEXT* context) {
    context->orig_x0 = context->regs[0];
}
