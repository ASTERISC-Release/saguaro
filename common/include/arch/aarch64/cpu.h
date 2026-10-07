/* SPDX-License-Identifier: LGPL-3.0-or-later */

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
 * everywhere Gramine used `get_tsc()`.
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

/* aarch64 has no equivalent of x86's split MFENCE/LFENCE/SFENCE; DMB SY orders all accesses
 * (loads and stores) both ways, so it is used for all three. */
#define MB()  __asm__ __volatile__("dmb sy" ::: "memory")
#define RMB() __asm__ __volatile__("dmb sy" ::: "memory")
#define WMB() __asm__ __volatile__("dmb sy" ::: "memory")
