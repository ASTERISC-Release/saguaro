/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/*
 * riscv64 CPU information for the PAL: fills `struct pal_cpu_info` (vendor/brand/family/model/
 * stepping strings shared with x86_64's CPUID-derived scheme).
 *
 * riscv64 has no CPUID instruction. The hardware identity on RISC-V Linux lives in the device
 * tree / `/proc/cpuinfo` per-hart fields (`isa`, `mmu`, `uarch` -- e.g.
 * `uarch : riscv,isa ... ; c910` or `uarch : rv64gc` style strings, vendor-part information is
 * only available via `mvendorid`/`marchid`/`mimpid` CSRs, which are supervisor-mode and not
 * readable from user mode). The aarch64 sibling decoded MIDR_EL1 (readable at EL0 there);
 * riscv64 has no user-mode ID-register read at all, so this parses the host's /proc/cpuinfo
 * `uarch`/`isa` lines instead.
 *
 * TODO(Phase 3): implement the host /proc/cpuinfo parse (which hart's block is read is best
 * keyed by sched_getcpu()/the boot hart; QEMU virt reports one uniform `uarch` for all harts
 * anyway) and map it onto vendor/brand. Until then this is a link-passing Phase-1 stub that
 * aborts on first use (Documentation/devel/riscv/plan.md, Phase 1: "stubs that abort").
 */

#include "pal_internal.h"

int _PalGetCPUInfo(struct pal_cpu_info* info) {
    BUG();
    return 0;
}
