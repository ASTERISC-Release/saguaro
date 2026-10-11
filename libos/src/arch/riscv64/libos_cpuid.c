/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/*
 * riscv64 CPU feature flags (port of the aarch64 libos_cpuid.c).
 *
 * PHASE-1 STUB: aborts. Phase 4 implements this by reading AT_HWCAP/AT_HWCAP2 from
 * /proc/self/auxv and mapping the riscv_hwcap single-letter extension bits onto the same
 * lowercase flag names riscv64 Linux's own /proc/cpuinfo "isa" line spells (see the doc comment
 * in libos/include/arch/riscv64/libos_cpuid.h). The vector ('v') bit must be masked out of
 * anything reported: the port handles F/D only and hides V from applications (plan.md
 * decisions), and the PAL side exposes the same masking so /proc/cpuinfo stays coherent with
 * what the LibOS will actually let an application use via signals/context (the riscv_v_ext
 * records in pal/include/arch/riscv64/linux/ucontext.h are never parsed or synthesized).
 */

#include "libos_cpuid.h"
#include "libos_internal.h"

int libos_get_cpu_flags(char** out_cpu_flags) {
    BUG();
}
