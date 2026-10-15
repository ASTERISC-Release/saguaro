/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/*
 * AArch64 host-arch definitions of the Windows VBS-enclave PAL. As in the direct PAL, a
 * Gramine-internal thread keeps its PAL_TCB in TPIDR_EL0, tagged with bit 0 (an application thread
 * keeps its own TLS there and is found through the registry; see pal_arch.h). TPIDR_EL0 is writable
 * inside a VBS enclave and kept per thread (phase 0, step d).
 */

#pragma once

#ifdef IN_PAL

#include "api.h"
#include "cpu.h"

#define ARCH_HIGHEST_ADDR ((1ul << 47) - PAGE_SIZE)

static inline int pal_set_tcb(PAL_TCB* tcb) {
    __asm__ volatile("msr tpidr_el0, %0" : : "r"((uint64_t)tcb | 0x1));
    return 0;
}

#endif /* IN_PAL */
