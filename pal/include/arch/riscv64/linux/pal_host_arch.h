/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/*
 * This file contains Linux on riscv64 specific functions related to the PAL.
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
 * Conservative upper bound on usable userspace VA. riscv64 has no single architectural constant
 * here -- the real limit depends on the kernel's paging mode (sv39: 39-bit VA, sv48: 48-bit,
 * sv57: 57-bit; the upper half of the VA space is the sign-extended kernel window, so user VA is
 * [0, 2^(bits-1)) and sv48 -- what Ubuntu's riscv64 kernels use -- gives exactly 47 usable bits).
 * We deliberately pick the same conservative 47-bit value as x86_64 and aarch64: it is exact for
 * the sv48 guest and a safe underestimate for sv57 (this is only used as a MIN() clamp against
 * the real queried highest mapped address in pal_memory.c, so underestimating costs a little
 * range, never correctness; sv39 kernels would *overestimate* -- the clamp's other input, the
 * kernel-reported mmap range, keeps the result correct there too).
 */
#define ARCH_HIGHEST_ADDR ((1ul << 47) - PAGE_SIZE)

/*
 * The thread pointer `tp` is ordinary user-writable architectural state -- `mv tp, reg` needs no
 * syscall (unlike x86_64's arch_prctl(ARCH_SET_FS)) -- but exactly like aarch64's TPIDR_EL0 it is
 * the one such register and the application's own libc TLS needs it, so the PAL cannot *keep*
 * PAL_TCB there. pal_set_tcb() therefore tags PAL_TCB with bit 0 and installs it in `tp`, giving
 * *this* thread an immediately correct, independent-of-every-other-thread PAL_TCB for as long as
 * nothing else overwrites `tp` (forever for Gramine's own internal threads; until the app's TLS
 * setup takes over for the thread that runs application code -- see
 * pal_tcb_singleton_snapshot_before_user_entry() and the per-thread registry in
 * pal_arch_exception.c). See findings #9, #12, #13 in the aarch64 port log.
 */
static inline int pal_set_tcb(PAL_TCB* tcb) {
    __asm__ volatile("mv tp, %0" : : "r"((uint64_t)tcb | 0x1));
    return 0;
}

#endif /* IN_PAL */
