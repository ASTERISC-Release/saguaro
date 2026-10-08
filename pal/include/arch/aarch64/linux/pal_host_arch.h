/* SPDX-License-Identifier: LGPL-3.0-or-later */

/*
 * This file contains Linux on aarch64 specific functions related to the PAL.
 */

#pragma once

#ifdef IN_PAL

#include "api.h"
#include "cpu.h"
/* Many generic (non-arch-dir) PAL files reach DO_SYSCALL only transitively, via
 * pal.h -> pal_arch.h -> this file -- same as x86_64's pal_host_arch.h, which also includes this
 * directly rather than relying on each call site to do so itself. */
#include "syscall.h"

/*
 * Conservative upper bound on usable userspace VA. x86_64 uses the hard 47-bit canonical limit
 * of CPUs without 5-level paging (see the LA57 comment in pal_main.c); aarch64 has no single
 * architectural constant here -- the real limit depends on the kernel's configured VA_BITS
 * (commonly 48, but 39 on some configs). We deliberately pick the same conservative 47-bit value
 * as x86_64: it's a safe underestimate on every aarch64 kernel we expect to run on (this is only
 * used as a MIN() clamp against the real queried highest mapped address in pal_memory.c, so
 * underestimating costs a little range, never correctness).
 */
#define ARCH_HIGHEST_ADDR ((1ul << 47) - PAGE_SIZE)

/* Milestone A stopgap (see the long comment on g_pal_tcb_singleton in pal_arch.h): PAL_TCB is a
 * plain process-wide global here, not reached via a register, because TPIDR_EL0 is needed for
 * the application's own libc TLS pointer and aarch64 has no second EL0-writable register the
 * way x86_64 has %gs free alongside %fs. So, unlike x86_64's pal_set_tcb() (which must issue
 * arch_prctl(ARCH_SET_GS, ...) because %gs isn't writable directly from userspace), this simply
 * stores the pointer -- no register, no syscall, and deliberately no interaction with TPIDR_EL0
 * at all, leaving it entirely under the application's own libc's control. */
static inline int pal_set_tcb(PAL_TCB* tcb) {
    g_pal_tcb_singleton = tcb;
    return 0;
}

#endif /* IN_PAL */
