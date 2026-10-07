/* SPDX-License-Identifier: LGPL-3.0-or-later */

/*
 * This file contains Linux on aarch64 specific functions related to the PAL.
 */

#pragma once

#ifdef IN_PAL

#include "api.h"
#include "cpu.h"

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

/* Gramine uses GCC's stack protector that looks for the canary at tpidr_el0:[0x8] (see
 * pal_arch.h), but this function changes TPIDR_EL0, so we disable stack protector here (even
 * though it is mostly inlined).
 *
 * Unlike x86_64 (which needs the arch_prctl(ARCH_SET_GS, ...) syscall because %gs cannot be
 * written directly from userspace), aarch64's TPIDR_EL0 is a normal EL0-writable system
 * register -- no syscall is needed at all. */
__attribute_no_stack_protector
static inline int pal_set_tcb(PAL_TCB* tcb) {
    __asm__ volatile("msr tpidr_el0, %0" :: "r"(tcb) : "memory");
    return 0;
}

#endif /* IN_PAL */
