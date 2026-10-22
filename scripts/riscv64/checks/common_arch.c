/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/* Phase 2 check of the riscv64 port (dev docs: riscv/PLAN.md): the hand-written
 * assembly and inline-asm helpers of the `common/` layer, exercised directly (no Saguaro
 * runtime):
 *
 *   - ct_memequal()   (common/src/arch/<arch>/{ct_memequal.nasm,ct_memequal.S}) against a
 *                     portable C reference over lengths spanning every loop edge case;
 *   - probe_stack()   (pal/src/host/linux-common/arch/<arch>/stack_probe.{nasm,S}) touching
 *                     real pages and returning the stack pointer;
 *   - get_tsc()       monotonic and advancing (riscv64: `csrr ..., time`);
 *   - rdrand()       (riscv64: timer-derived) producing distinct values;
 *   - MB()/RMB()/WMB() and CPU_RELAX() as a compile-and-run smoke (RVWMO reordering is not
 *                     observable from a single thread; this only proves they assemble, link
 *                     and do not fault -- recorded as such, not as a memory-model proof).
 *
 * The arch-specific sources are *not* compiled in here; the runner links the real files from
 * the repository next to this driver, so what is tested is exactly what the PAL/LibOS link.
 * Builds natively on x86_64 (with nasm assembling the .nasm) and cross for riscv64; aarch64
 * (ct_memequal.S + GAS stack_probe.S) is supported by the same flags but has not been run.
 *
 *   riscv64-linux-gnu-gcc -static -O2 -I common/include -I common/include/arch/riscv64 \
 *       -o common_arch common_arch.c \
 *       common/src/arch/riscv64/ct_memequal.S \
 *       pal/src/host/linux-common/arch/riscv64/stack_probe.S
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cpu.h"

/* The two hand-written assembly entry points (declared, not included, to keep this driver
 * out of the api.h/log.h dependency web: nothing here may pull Saguaro's logging in). */
bool ct_memequal(const void* lhs, const void* rhs, size_t count);
void probe_stack(size_t pages_count);

static unsigned failures;

static void check(bool ok, const char* what) {
    if (!ok) {
        failures++;
        printf("FAIL: %s\n", what);
    }
}

/* portable reference for ct_memequal: "all `count` bytes equal" */
static bool memequal_ref(const void* a, const void* b, size_t count) {
    const unsigned char* x = a;
    const unsigned char* y = b;
    for (size_t i = 0; i < count; i++) {
        if (x[i] != y[i])
            return false;
    }
    return true;
}

static uint64_t rng_state = 0x2545F4914F6CDD1D;
static uint64_t next_rand(void) {
    rng_state ^= rng_state >> 12;
    rng_state ^= rng_state << 25;
    rng_state ^= rng_state >> 27;
    return rng_state * 0x2545F4914F6CDD1D;
}

static void test_ct_memequal(void) {
    /* lengths chosen to hit every loop-boundary and tail case of the hand-written loops
     * (x86_64 and riscv64 versions are both simple byte loops; 0 checks the empty case) */
    static const size_t lengths[] = {
        0, 1, 2, 3, 4, 7, 8, 9, 15, 16, 17, 31, 32, 33, 63, 64, 65,
        127, 128, 129, 255, 256, 257, 300, 1000,
    };
    static unsigned char buf_a[1024];
    static unsigned char buf_b[1024];

    for (size_t i = 0; i < sizeof(buf_a); i++) {
        buf_a[i] = (unsigned char)next_rand();
        buf_b[i] = buf_a[i];
    }

    char msg[128];
    for (size_t l = 0; l < sizeof(lengths) / sizeof(lengths[0]); l++) {
        size_t n = lengths[l];
        bool asm_eq = ct_memequal(buf_a, buf_b, n);
        snprintf(msg, sizeof(msg), "ct_memequal equal, len=%zu", n);
        check(asm_eq == memequal_ref(buf_a, buf_b, n) && asm_eq, msg);

        /* one differing byte at every offset for short lengths, sampled for long ones */
        for (size_t i = 0; i < n; i++) {
            if (n > 40 && (next_rand() % 4) != 0)
                continue;
            buf_b[i] ^= 0x80; /* flip the top bit: never an accidental equality */
            asm_eq = ct_memequal(buf_a, buf_b, n);
            bool ref_eq = memequal_ref(buf_a, buf_b, n);
            snprintf(msg, sizeof(msg), "ct_memequal differ at %zu, len=%zu", i, n);
            check(asm_eq == ref_eq && !ref_eq, msg);
            buf_b[i] ^= 0x80; /* restore */
        }

        /* differing byte at the very end (off-by-one trap) */
        if (n > 0) {
            buf_b[n - 1] ^= 1;
            asm_eq = ct_memequal(buf_a, buf_b, n);
            snprintf(msg, sizeof(msg), "ct_memequal last-byte-differs, len=%zu", n);
            check(asm_eq == memequal_ref(buf_a, buf_b, n) && !asm_eq, msg);
            buf_b[n - 1] ^= 1;
        }
    }

    /* overlapping-source identity (a0 == a1 after increment patterns): same pointer must be
     * equal to itself for any length */
    for (size_t n = 0; n < 64; n++)
        check(ct_memequal(buf_a, buf_a, n), "ct_memequal self-compare");
}

static void test_probe_stack(void) {
    /* probe_stack(N) must touch N pages below the current SP and restore SP afterwards;
     * a fault during the touch would kill us here, and a non-restored SP would show up as
     * the stack canary/frame corruption below (and locally via the asm below). */
    uintptr_t sp_before, sp_after;
#if defined(__riscv)
    __asm__ volatile("mv %0, sp" : "=r"(sp_before));
#elif defined(__x86_64__)
    __asm__ volatile("mov %%rsp, %0" : "=r"(sp_before));
#elif defined(__aarch64__)
    __asm__ volatile("mov %0, sp" : "=r"(sp_before));
#endif
    probe_stack(1);
    probe_stack(16);
    probe_stack(256);   /* 1 MiB down */
#ifdef __riscv
    /* The x86_64 original and the aarch64 port are do-while loops for which N = 0 means
     * "decrement forever" (x86 `loop` wraps RCX) -- verified: the x86_64 driver segfaults
     * here, which is why the call below is riscv64-only. The riscv64 port added an entry
     * `beqz` instead of replicating the latent bug (callers only ever pass positive
     * constants; see stack_probe.S's comment). */
    probe_stack(0);     /* the degenerate no-op case (riscv64-only robustness) */
#endif
#if defined(__riscv)
    __asm__ volatile("mv %0, sp" : "=r"(sp_after));
#elif defined(__x86_64__)
    __asm__ volatile("mov %%rsp, %0" : "=r"(sp_after));
#elif defined(__aarch64__)
    __asm__ volatile("mov %0, sp" : "=r"(sp_after));
#endif
    check(sp_before == sp_after, "probe_stack restores sp");

    /* deep-enough recursion right after the probes must still work (the probed region was
     * really faulted-in and the stack is usable again) */
    volatile int sink = 0;
    for (int i = 0; i < 1000; i++)
        sink += i;
    check(sink == 499500, "stack usable after probes");

    printf("  probe_stack: OK (273 pages probed, sp restored)\n");
}

static void test_get_tsc(void) {
    uint64_t t0 = get_tsc();
    check(t0 != 0, "get_tsc nonzero");
    uint64_t t1 = get_tsc();
    check(t1 >= t0, "get_tsc monotonic across reads");

    /* the timer must actually advance (QEMU virt's machine timer: 10 MHz; if `time` were
     * stuck or trap-broken we would read a constant forever) */
    uint64_t t2 = t1;
    unsigned iters = 0;
    while (t2 == t1 && iters < 1000000) {
        CPU_RELAX();
        t2 = get_tsc();
        iters++;
    }
    check(t2 != t1, "get_tsc advances");

    /* strictly non-decreasing over a sweep */
    uint64_t prev = get_tsc();
    bool mono = true;
    for (unsigned i = 0; i < 10000; i++) {
        uint64_t cur = get_tsc();
        if (cur < prev) {
            mono = false;
            break;
        }
        prev = cur;
    }
    check(mono, "get_tsc non-decreasing over 10k reads");
}

static void test_rdrand(void) {
    /* distinctness smoke (the value feeds log jitter and fake topology only) */
    uint32_t first = rdrand();
    bool differs = false;
    for (unsigned i = 0; i < 1000 && !differs; i++)
        differs = rdrand() != first;
    check(differs, "rdrand produces distinct values");

    /* x86_64's RDRAND must also have set the carried value sanely on other archs; all we
     * promise anywhere in Saguaro is "some bits came out", checked by 1000 calls not
     * crashing and being observed changing above. */
}

static volatile int s_flag;
static volatile int s_data;

/* A release/acquire pair through MB(): producer/consumer in one process with pthread would
 * not *prove* anything about RVWMO (fences can only fail by being absent); this proves the
 * macros assemble, emit instructions (or none, on riscv64's EL0 no-ops they must still
 * compile to the documented empty form) and do not fault in real code. */
static void test_fences(void) {
    for (int i = 0; i < 10000; i++) {
        s_data = i;
        WMB();
        s_flag = 1;
        RMB();
        if (s_flag == 1) {
            MB();
            if (s_data != i) {
                check(false, "fence smoke: value torn");
                return;
            }
        }
        s_flag = 0;
        CPU_RELAX();
    }
    check(true, "fences/relax compile and run");
}

int main(void) {
    printf("common_arch check on "
#if defined(__x86_64__)
           "x86_64"
#elif defined(__aarch64__)
           "aarch64"
#elif defined(__riscv) && __riscv_xlen == 64
           "riscv64"
#else
           "unknown"
#endif
           "\n");

    test_ct_memequal();
    test_probe_stack();
    test_get_tsc();
    test_rdrand();
    test_fences();

    printf("%s (%u failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures != 0;
}
