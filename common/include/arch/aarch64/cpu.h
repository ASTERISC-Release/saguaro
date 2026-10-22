/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

#pragma once

#include <stdint.h>
#include <stdnoreturn.h>

#define PAGE_SIZE       (1ul << 12)
#define PRESET_PAGESIZE PAGE_SIZE

/*!
 * \brief Read the architected generic counter (CNTVCT_EL0).
 *
 * This is the aarch64 analog of x86_64's RDTSC; it is the free-running counter underlying the
 * generic timer, not a CPU-cycle counter, but it serves the same "cheap monotonic counter" role
 * everywhere Saguaro used `get_tsc()`.
 */
static inline uint64_t get_tsc(void) {
    uint64_t val;
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(val));
    return val;
}

/* This function must be breakable for debugging and GDB integration scripts (e.g. see
 * `fork_and_access_file.gdb` in LibOS regression tests). */
__attribute__((__noinline__)) __attribute__((unused))
static noreturn void die_or_inf_loop(void) {
    __asm__ volatile (
        "1: \n"
        "brk #0 \n"
        "b 1b \n"
    );
    __builtin_unreachable();
}

#define CPU_RELAX() __asm__ volatile("yield" ::: "memory")

/*
 * Interrupt masking. x86's `sti`/`cli` map onto the PSTATE.{D,A,I,F} bits, moved as a unit
 * between a GPR and PSTATE via MRS/MSR #DAIF. We mask/unmask all four (value 0xf), matching
 * how the Saguaro vm kernel uses sti/cli around its scheduler critical sections; note that
 * these only have effect at EL1 (the vm unikernel kernel) -- at EL0 the writes are trapped or
 * ignored, exactly like `sti`/`cli` faulting in user mode on x86.
 */
static inline void cli(void) {
    __asm__ volatile("msr daifset, #0xf" ::: "memory");
}

static inline void sti(void) {
    __asm__ volatile("msr daifclr, #0xf" ::: "memory");
}

/*
 * Random 32-bit value. x86's rdrand() is a single `rdrand` instruction whose "data valid" flag
 * is ignored by all Saguaro call sites (they use it for log jitter, canary mixing etc., not for
 * security). The aarch64 analog is ARMv8.5-RNG's RNDR (`mrs x, rndr`, C-flag signals validity);
 * it is present on QEMU virt with `-cpu max` and on all Armv8.5+ cores. On a pre-8.5 core the
 * `mrs rndr` would be an undefined instruction -- acceptable for the same reason the x86 code
 * assumes RDRAND is present, and any security-grade randomness must come from elsewhere.
 */
static inline uint32_t rdrand(void) {
#ifdef __ARM_FEATURE_RNG
    uint64_t val;
    __asm__ volatile("mrs %0, rndr" : "=r"(val));
    return (uint32_t)val;
#else
    /* No ARMv8.5-RNG at compile time (the kernel is built for plain armv8-a): the value only
     * feeds log jitter / fake-topology filler (see call sites), never security. A mix of the
     * generic counter keeps callers getting distinct values without an undefined `mrs rndr`. */
    uint64_t val;
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(val));
    val = (val >> 4) ^ 0x9e3779b97f4a7c15UL;
    return (uint32_t)(val >> 32) ^ (uint32_t)val;
#endif
}

/* aarch64 has no equivalent of x86's split MFENCE/LFENCE/SFENCE; DMB SY orders all accesses
 * (loads and stores) both ways, so it is used for all three. */
#define MB()  __asm__ __volatile__("dmb sy" ::: "memory")
#define RMB() __asm__ __volatile__("dmb sy" ::: "memory")
#define WMB() __asm__ __volatile__("dmb sy" ::: "memory")
