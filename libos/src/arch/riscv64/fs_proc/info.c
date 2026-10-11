/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/*
 * riscv64 /proc/cpuinfo rendering (port of the aarch64 fs_proc/info.c).
 *
 * PHASE-1 STUB: aborts. Phase 4 implements the riscv64 flavor of /proc/cpuinfo: the kernel
 * prints one `processor` block per hart with `isa` (the full rv64imafd..._zicsr_... extension
 * string), `mmu`, `uarch` and a hartid-indexed `hart` line, rather than x86's vendor/family/
 * model/stepping/microcode blocks or aarch64's CPU-implementer/part lines. The values come from
 * the shared `struct pal_cpu_info` filled by the PAL's _PalGetCPUInfo() (see
 * pal/src/arch/riscv64/pal_cpuid.c, itself a Phase-3 item).
 */

#include "libos_fs_proc.h"

int proc_cpuinfo_display_cpu(char** str, size_t* size, size_t* max,
                             const struct pal_topo_info* topo,
                             const struct pal_cpu_info* cpu, size_t i,
                             struct pal_cpu_thread_info* thread) {
    BUG();
}

int proc_cpuinfo_display_tail(char** str, size_t* size, size_t* max,
                              const struct pal_cpu_info* cpu) {
    BUG();
}
