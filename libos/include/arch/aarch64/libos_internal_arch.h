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
