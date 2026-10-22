/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

#pragma once

#include <stdint.h>
#include <stdnoreturn.h>

#define PAGE_SIZE       (1ul << 12)
#define PRESET_PAGESIZE PAGE_SIZE

/*!
 * \brief Read the machine timer (`time` CSR, read-only from user mode via `rdtime`).
 *
 * This is the riscv64 analog of x86_64's RDTSC and aarch64's CNTVCT_EL0 read: a free-running
 * monotonic counter (10 MHz on the QEMU `virt` platform, 25 MHz on many real SoCs), not a
 * CPU-cycle counter, but it serves the same "cheap monotonic counter" role everywhere Saguaro
 * used `get_tsc()`. Written as `csrr ..., time` (which the assembler encodes as `rdtime`): the
 * `time` CSR is architecturally read-only at supervisor/user level, and reading it never traps
 * when the Sstc/time delegation is in place, which every Linux riscv64 platform has.
 */
static inline uint64_t get_tsc(void) {
    uint64_t val;
    __asm__ volatile("csrr %0, time" : "=r"(val));
    return val;
}

/* This function must be breakable for debugging and GDB integration scripts (e.g. see
 * `fork_and_access_file.gdb` in LibOS regression tests). */
__attribute__((__noinline__)) __attribute__((unused))
static noreturn void die_or_inf_loop(void) {
    __asm__ volatile (
        "1: \n"
        "ebreak \n"
        "j 1b \n"
    );
    __builtin_unreachable();
}

/* The `pause` hint (Zihintpause) is not part of the rv64gc baseline this tree is compiled for
 * (Ubuntu's riscv64 gcc defaults to rv64imafdc_zicsr_zifencei), so a bare compiler barrier is
 * used instead -- a spin-loop hint only, its absence costs nothing but micro-optimization.
 * (AArch64's `yield` has the same status there.) */
#define CPU_RELAX() __asm__ volatile("" ::: "memory")

/*
 * Interrupt masking. On x86_64/aarch64 these map onto ring-0 interrupt flags (RFLAGS.IF /
 * PSTATE.{D,A,I,F}) that Saguaro's vm unikernel kernel manipulates at EL1/ring-0; riscv64 has no
 * user-mode-accessible interrupt mask (sstatus.SIE is supervisor state, unreachable from EL0), so
 * these compile to nothing. They are kept only because the shared vm-PAL sources reference them;
 * a future riscv64 vm-PAL will need a different mechanism (e.g. SBI or a software mask).
 */
static inline void cli(void) {
    __asm__ volatile("" ::: "memory");
}

static inline void sti(void) {
    __asm__ volatile("" ::: "memory");
}

/*
 * Random 32-bit value. x86_64's rdrand()/aarch64's RNDR have no riscv64-baseline equivalent: the
 * Zkr extension (ZCRNG) exists but is not compiled in here (rv64gc baseline) and is not
 * implemented by the QEMU `virt` CPU we test on. The value only feeds log jitter / fake-topology
 * filler (see call sites), never security -- same rationale as the aarch64 fallback path. A mix
 * of the machine timer keeps callers getting distinct values without an illegal instruction.
 */
static inline uint32_t rdrand(void) {
    uint64_t val;
    __asm__ volatile("csrr %0, time" : "=r"(val));
    val = (val >> 4) ^ 0x9e3779b97f4a7c15UL;
    return (uint32_t)(val >> 32) ^ (uint32_t)val;
}

/*
 * RISC-V memory model (RVWMO): `fence` with predecessor/successor sets. There is no x86-style
 * split fence taxonomy; the full `fence iorw, iorw` orders all loads and stores both ways (the
 * direct analog of aarch64's `dmb sy` used for all three) and is used here for all three for the
 * same reason -- no call site distinguishes them.
 */
#define MB()  __asm__ __volatile__("fence iorw, iorw" ::: "memory")
#define RMB() __asm__ __volatile__("fence iorw, iorw" ::: "memory")
#define WMB() __asm__ __volatile__("fence iorw, iorw" ::: "memory")
