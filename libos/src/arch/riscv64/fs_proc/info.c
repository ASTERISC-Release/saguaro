/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */
/* Copyright (C) 2014 Stony Brook University
 * Copyright (C) 2022 IBM Corporation
 */

/*
 * riscv64 /proc/cpuinfo rendering (port of the aarch64 fs_proc/info.c).
 *
 * Follows the riscv64 kernel's arch/riscv/kernel/cpuinfo.c `c_show()` flavor: one block per
 * hart with `processor`, `hart`, `isa`, `mmu` and the machine-CPU identity triplet
 * (`mvendorid`/`marchid`/`mimpid`, which the kernel reads from the EL0-invisible CSRs),
 * separated by a blank line -- no x86 vendor/family/model/stepping block, no aarch64
 * implementer/part lines, and no BogoMIPS (the riscv64 kernel computes none; see
 * pal/src/arch/riscv64/pal_cpuid.c). The `isa`/`mmu` strings come verbatim from
 * `struct pal_cpu_info` (`cpu_brand`/`cpu_mmu`), which the linux-PAL side filled from the
 * host's own cpuinfo with all vector tokens stripped (plan.md decision: F/D only, V hidden).
 *
 * The one thing deliberately *not* reproduced: newer riscv64 kernels append a summary section
 * headed by a `hart isa` line (and per-hart `<id> rv64imafdc...` lines) after the per-block
 * output. Gramine's synthesized file omits it -- the regression suite's cpuinfo test parses
 * the last blank-line-separated block as `key: value` pairs, which that colon-less section
 * would break, and nothing else reads it.
 */

#include "api.h"
#include "libos_fs_proc.h"
#include "pal.h"

#define ADD_INFO(fmt, ...)                                            \
    do {                                                              \
        int ret = print_to_str(str, *size, max, fmt, ##__VA_ARGS__);  \
        if (ret < 0) {                                                \
            return ret;                                               \
        }                                                             \
        *size += ret;                                                 \
    } while (0)

int proc_cpuinfo_display_cpu(char** str, size_t* size, size_t* max,
                             const struct pal_topo_info* topo,
                             const struct pal_cpu_info* cpu, size_t i,
                             struct pal_cpu_thread_info* thread) {
    __UNUSED(topo);
    __UNUSED(thread);

    /* `hart` is the hardware thread id the kernel prints; Gramine has no access to the physical
     * hart ids and renders the linear thread index instead -- identical on the FDT-enumerated
     * platforms riscv64 Linux boots on, where hart ids are assigned 0..n-1 at boot. */
    ADD_INFO("processor\t: %lu\n", i);
    ADD_INFO("hart\t\t: %lu\n", i);
    ADD_INFO("isa\t\t: %s\n", cpu->cpu_brand);
    ADD_INFO("mmu\t\t: %s\n", cpu->cpu_mmu);

    /* The privilege-spec identity triplet (see the file comment). The kernel prints these in
     * hex; `cpu_family`/`cpu_model`/`cpu_stepping` carry mvendorid/marchid/mimpid on this
     * architecture (see pal_cpuid.c for the mapping). All zero on QEMU `virt`. */
    ADD_INFO("mvendorid\t: 0x%lx\n", (unsigned long)cpu->cpu_family);
    ADD_INFO("marchid\t\t: 0x%lx\n", (unsigned long)cpu->cpu_model);
    ADD_INFO("mimpid\t\t: 0x%lx\n", (unsigned long)cpu->cpu_stepping);

    ADD_INFO("\n");

    return 0;
}

int proc_cpuinfo_display_tail(char** str, size_t* size, size_t* max,
                              const struct pal_cpu_info* cpu) {
    __UNUSED(str);
    __UNUSED(size);
    __UNUSED(max);
    __UNUSED(cpu);

    return 0;
}
