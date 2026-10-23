/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2023 Intel Corporation */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/*
 * System call entry -- aarch64 counterpart of vm-common/kernel_syscalls.c.
 *
 * x86_64 wires its syscall entry into three MSRs (STAR/LSTAR/SYSCALL_MASK) so that the
 * `syscall` instruction lands in `syscall_asm`. aarch64 has a single synchronous-exception
 * vector table (VBAR_EL1) selected by one register write; the `svc` instruction enters it at
 * the "Current EL using SP_ELx, sync" slot, and vector selection inside the table is a fixed
 * 128-byte-per-entries layout -- no selector or RFLAGS-mask configuration exists (PSTATE bits
 * are saved into SPSR_EL1 by hardware instead). Everything arch-specific thus collapses to:
 * point VBAR_EL1 at the table defined in arch/aarch64/kernel_context.S and make sure the table
 * is visible with the MMU on (identity-mapped kernel, so its link address is its load address).
 */

#include <stdint.h>

#include "api.h"

#include "kernel_syscalls.h"

extern char vectors[]; /* in kernel_context.S */

int syscalls_init(void) {
    __asm__ volatile("msr vbar_el1, %0" :: "r"(vectors));
    __asm__ volatile("isb" ::: "memory");
    return 0;
}
