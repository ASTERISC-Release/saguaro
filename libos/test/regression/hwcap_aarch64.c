/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/*
 * AT_HWCAP / AT_HWCAP2 on AArch64 (dev docs: aarch64/hwcap/PLAN.md): reads both words
 * with getauxval() and, for every advertised feature that has a probe, executes one instruction
 * of that class under a SIGILL handler, checking the result where it is cheap to know. Fails if
 * a probe faults or computes a wrong result, if FP or ASIMD is missing (the LibOS gave no words at
 * all), or if a bit outside the allowlist of the PALs (pal/include/arch/aarch64/pal_hwcap.h) is
 * advertised. Allowlisted features that are not advertised are only reported.
 *
 * With the argument "--all" every probe runs, advertised or not, and only the outcome is printed
 * (to see what the CPU executes; a SIGILL there is not a failure).
 *
 * Under saguaro-vm an illegal instruction at EL0 stops the whole VM instead of raising SIGILL, so
 * there the test reaching its end is the evidence.
 */

#define _GNU_SOURCE
#include <setjmp.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>

/* the probes need the assembler to accept every extension; the compiler's own code stays at its
 * default target */
#define ARCH ".arch armv8.6-a+crypto+sha3+sm4+fp16+fp16fml+rng+memtag\n"

/* bits of Linux's uapi <asm/hwcap.h>, spelled here so the test does not depend on the version of
 * the headers it is built with */
#define B(n) (1UL << (n))

/* must match PAL_HWCAP_ALLOWED / PAL_HWCAP2_ALLOWED in pal/include/arch/aarch64/pal_hwcap.h */
#define ALLOWED_HWCAP  (B(0) | B(1) | B(2) | B(3) | B(4) | B(5) | B(6) | B(7) | B(8) | B(9) | B(10) \
                        | B(11) | B(12) | B(13) | B(14) | B(15) | B(16) | B(17) | B(18) | B(19)    \
                        | B(20) | B(21) | B(23) | B(25) | B(26) | B(27) | B(29))
#define ALLOWED_HWCAP2 (B(0) | B(7) | B(8) | B(13) | B(14) | B(15) | B(16))

static sigjmp_buf g_jmp;
static volatile sig_atomic_t g_in_probe;

static void sigill_handler(int sig) {
    (void)sig;
    if (!g_in_probe)
        abort();
    siglongjmp(g_jmp, 1);
}

typedef bool (*probe_fn)(void);

static uint64_t g_word = 40;
static uint8_t g_bytes[64] __attribute__((aligned(64)));

static bool p_fp(void) {
    double a = 1.5, b = 2.25, r;
    __asm__ volatile("fadd %d0, %d1, %d2" : "=w"(r) : "w"(a), "w"(b));
    return r == 3.75;
}

static bool p_asimd(void) {
    uint32_t a[4] = {1, 2, 3, 4}, b[4] = {10, 20, 30, 40}, r[4];
    __asm__ volatile("ld1 {v0.4s}, [%1]\n ld1 {v1.4s}, [%2]\n add v0.4s, v0.4s, v1.4s\n"
                     "st1 {v0.4s}, [%0]" : : "r"(r), "r"(a), "r"(b) : "v0", "v1", "memory");
    return r[0] == 11 && r[3] == 44;
}

static bool p_aes(void) {
    /* AESE with zero state and key: SubBytes(0) = 0x63 in every byte; MixColumns keeps a column
     * of equal bytes */
    uint8_t r[16];
    __asm__ volatile(ARCH "movi v0.16b, #0\n movi v1.16b, #0\n aese v0.16b, v1.16b\n"
                     "aesmc v0.16b, v0.16b\n st1 {v0.16b}, [%0]" : : "r"(r) : "v0", "v1", "memory");
    for (int i = 0; i < 16; i++)
        if (r[i] != 0x63)
            return false;
    return true;
}

static bool p_pmull(void) {
    /* (x + 1) * (x + 1) = x^2 + 1 over GF(2) */
    uint64_t r[2];
    __asm__ volatile(ARCH "mov x9, #3\n fmov d1, x9\n fmov d2, x9\n pmull v0.1q, v1.1d, v2.1d\n"
                     "st1 {v0.2d}, [%0]" : : "r"(r) : "x9", "v0", "v1", "v2", "memory");
    return r[0] == 5 && r[1] == 0;
}

static bool p_sha1(void) {
    uint32_t r;
    __asm__ volatile(ARCH "mov w9, #1\n fmov s1, w9\n sha1h s0, s1\n fmov %w0, s0"
                     : "=r"(r) : : "x9", "v0", "v1");
    return r == 0x40000000; /* rotate left by 30 */
}

static bool p_sha2(void) {
    __asm__ volatile(ARCH "movi v0.16b, #1\n movi v1.16b, #2\n movi v2.16b, #3\n"
                     "sha256h q0, q1, v2.4s\n sha256su0 v0.4s, v1.4s" : : : "v0", "v1", "v2");
    return true;
}

static bool p_sha512(void) {
    __asm__ volatile(ARCH "movi v0.16b, #1\n movi v1.16b, #2\n movi v2.16b, #3\n"
                     "sha512h q0, q1, v2.2d" : : : "v0", "v1", "v2");
    return true;
}

static bool p_sha3(void) {
    uint8_t r[16];
    __asm__ volatile(ARCH "movi v1.16b, #1\n movi v2.16b, #2\n movi v3.16b, #4\n"
                     "eor3 v0.16b, v1.16b, v2.16b, v3.16b\n st1 {v0.16b}, [%0]"
                     : : "r"(r) : "v0", "v1", "v2", "v3", "memory");
    return r[0] == 7 && r[15] == 7;
}

static bool p_sm3(void) {
    __asm__ volatile(ARCH "movi v1.16b, #1\n movi v2.16b, #2\n movi v3.16b, #3\n"
                     "sm3ss1 v0.4s, v1.4s, v2.4s, v3.4s" : : : "v0", "v1", "v2", "v3");
    return true;
}

static bool p_sm4(void) {
    __asm__ volatile(ARCH "movi v0.16b, #1\n movi v1.16b, #2\n sm4e v0.4s, v1.4s" : : : "v0", "v1");
    return true;
}

static uint32_t crc32_sw(uint32_t crc, uint64_t data) {
    for (int i = 0; i < 8; i++) {
        crc ^= (uint8_t)(data >> (8 * i));
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xedb88320u & -(crc & 1));
    }
    return crc;
}

static bool p_crc32(void) {
    uint32_t crc = 0xffffffff;
    uint64_t data = 0x0123456789abcdefULL;
    uint32_t r;
    __asm__ volatile(ARCH "crc32x %w0, %w1, %2" : "=r"(r) : "r"(crc), "r"(data));
    return r == crc32_sw(crc, data);
}

static bool p_atomics(void) {
    uint64_t old, swapped, cmp = 42;
    g_word = 40;
    __asm__ volatile(ARCH "mov x9, #2\n ldaddal x9, %0, [%1]" : "=&r"(old) : "r"(&g_word)
                     : "x9", "memory");
    if (old != 40 || g_word != 42)
        return false;
    __asm__ volatile(ARCH "mov x9, #7\n swpal x9, %0, [%1]" : "=&r"(swapped) : "r"(&g_word)
                     : "x9", "memory");
    if (swapped != 42 || g_word != 7)
        return false;
    cmp = 7;
    __asm__ volatile(ARCH "mov x9, #9\n casal %0, x9, [%1]" : "+r"(cmp) : "r"(&g_word)
                     : "x9", "memory");
    return cmp == 7 && g_word == 9;
}

static bool p_fphp(void) {
    uint16_t r;
    /* 1.0 + 2.0 = 3.0 in half precision (0x4200) */
    __asm__ volatile(ARCH "fmov h1, #1.0\n fmov h2, #2.0\n fadd h0, h1, h2\n umov %w0, v0.h[0]"
                     : "=r"(r) : : "v0", "v1", "v2");
    return r == 0x4200;
}

static bool p_asimdhp(void) {
    uint16_t r;
    __asm__ volatile(ARCH "fmov v1.8h, #1.0\n fmov v2.8h, #2.0\n fadd v0.8h, v1.8h, v2.8h\n"
                     "umov %w0, v0.h[7]" : "=r"(r) : : "v0", "v1", "v2");
    return r == 0x4200;
}

static bool p_cpuid(void) {
    /* EL0 read of an ID register, emulated by the kernel; its AES field must agree with HWCAP */
    uint64_t isar0;
    __asm__ volatile("mrs %0, s3_0_c0_c6_0" : "=r"(isar0));
    bool aes_reg = ((isar0 >> 4) & 0xf) >= 1;
    bool aes_cap = getauxval(AT_HWCAP) & B(3);
    printf("  ID_AA64ISAR0_EL1 = 0x%016lx\n", isar0);
    return aes_reg == aes_cap;
}

static bool p_asimdrdm(void) {
    __asm__ volatile(ARCH "movi v0.16b, #1\n movi v1.16b, #2\n movi v2.16b, #3\n"
                     "sqrdmlah v0.4s, v1.4s, v2.4s" : : : "v0", "v1", "v2");
    return true;
}

static bool p_jscvt(void) {
    double d = -3.75;
    int32_t r;
    __asm__ volatile(ARCH "fjcvtzs %w0, %d1" : "=r"(r) : "w"(d) : "cc");
    return r == -3;
}

static bool p_fcma(void) {
    __asm__ volatile(ARCH "movi v0.16b, #0\n fmov v1.4s, #1.0\n fmov v2.4s, #2.0\n"
                     "fcmla v0.4s, v1.4s, v2.4s, #90" : : : "v0", "v1", "v2");
    return true;
}

static bool p_lrcpc(void) {
    uint64_t r;
    g_word = 1234;
    __asm__ volatile(ARCH "ldapr %0, [%1]" : "=r"(r) : "r"(&g_word) : "memory");
    return r == 1234;
}

static bool p_ilrcpc(void) {
    uint64_t r;
    g_word = 5678;
    __asm__ volatile(ARCH "ldapur %0, [%1]" : "=r"(r) : "r"(&g_word) : "memory");
    return r == 5678;
}

static bool p_dcpop(void) {
    __asm__ volatile(ARCH "dc cvap, %0" : : "r"(g_bytes) : "memory");
    return true;
}

static bool p_dcpodp(void) {
    __asm__ volatile(ARCH "dc cvadp, %0" : : "r"(g_bytes) : "memory");
    return true;
}

static bool p_asimddp(void) {
    uint32_t r[4];
    __asm__ volatile(ARCH "movi v0.16b, #0\n movi v1.16b, #1\n movi v2.16b, #2\n"
                     "sdot v0.4s, v1.16b, v2.16b\n st1 {v0.4s}, [%0]"
                     : : "r"(r) : "v0", "v1", "v2", "memory");
    return r[0] == 8 && r[3] == 8;
}

static bool p_asimdfhm(void) {
    float r[4];
    __asm__ volatile(ARCH "movi v0.16b, #0\n fmov v1.8h, #1.0\n fmov v2.8h, #2.0\n"
                     "fmlal v0.4s, v1.4h, v2.4h\n st1 {v0.4s}, [%0]"
                     : : "r"(r) : "v0", "v1", "v2", "memory");
    return r[0] == 2.0f && r[3] == 2.0f;
}

static bool p_flagm(void) {
    __asm__ volatile(ARCH "cfinv\n cfinv\n mov x9, #0\n setf8 w9\n rmif x9, #0, #0"
                     : : : "x9", "cc");
    return true;
}

static bool p_flagm2(void) {
    __asm__ volatile(ARCH "axflag\n xaflag" : : : "cc");
    return true;
}

static bool p_frint(void) {
    double d = 2.5, r;
    __asm__ volatile(ARCH "frint32x %d0, %d1" : "=w"(r) : "w"(d));
    return r == 2.0; /* round to nearest even */
}

static bool p_sb(void) {
    __asm__ volatile(ARCH "sb" : : : "memory");
    return true;
}

static bool p_i8mm(void) {
    uint32_t r[4];
    __asm__ volatile(ARCH "movi v0.16b, #0\n movi v1.16b, #1\n movi v2.16b, #3\n"
                     "smmla v0.4s, v1.16b, v2.16b\n st1 {v0.4s}, [%0]"
                     : : "r"(r) : "v0", "v1", "v2", "memory");
    return r[0] == 24 && r[3] == 24; /* 8 products of 1 * 3 */
}

static bool p_bf16(void) {
    __asm__ volatile(ARCH "movi v0.16b, #0\n movi v1.16b, #1\n movi v2.16b, #2\n"
                     "bfdot v0.4s, v1.8h, v2.8h" : : : "v0", "v1", "v2");
    return true;
}

static bool p_dgh(void) {
    __asm__ volatile(ARCH "dgh" : : : "memory");
    return true;
}

static bool p_rng(void) {
    uint64_t r;
    __asm__ volatile(ARCH "mrs %0, s3_3_c2_c4_0" : "=r"(r) : : "cc"); /* RNDR */
    return true;
}

struct feature {
    int word; /* 1: AT_HWCAP, 2: AT_HWCAP2 */
    int bit;
    const char* name;
    probe_fn probe; /* NULL: a property, no instruction to try */
};

static const struct feature g_features[] = {
    {1, 0, "fp", p_fp},
    {1, 1, "asimd", p_asimd},
    {1, 2, "evtstrm", NULL},
    {1, 3, "aes", p_aes},
    {1, 4, "pmull", p_pmull},
    {1, 5, "sha1", p_sha1},
    {1, 6, "sha2", p_sha2},
    {1, 7, "crc32", p_crc32},
    {1, 8, "atomics", p_atomics},
    {1, 9, "fphp", p_fphp},
    {1, 10, "asimdhp", p_asimdhp},
    {1, 11, "cpuid", p_cpuid},
    {1, 12, "asimdrdm", p_asimdrdm},
    {1, 13, "jscvt", p_jscvt},
    {1, 14, "fcma", p_fcma},
    {1, 15, "lrcpc", p_lrcpc},
    {1, 16, "dcpop", p_dcpop},
    {1, 17, "sha3", p_sha3},
    {1, 18, "sm3", p_sm3},
    {1, 19, "sm4", p_sm4},
    {1, 20, "asimddp", p_asimddp},
    {1, 21, "sha512", p_sha512},
    {1, 23, "asimdfhm", p_asimdfhm},
    {1, 25, "uscat", NULL},
    {1, 26, "ilrcpc", p_ilrcpc},
    {1, 27, "flagm", p_flagm},
    {1, 29, "sb", p_sb},
    {2, 0, "dcpodp", p_dcpodp},
    {2, 7, "flagm2", p_flagm2},
    {2, 8, "frint", p_frint},
    {2, 13, "i8mm", p_i8mm},
    {2, 14, "bf16", p_bf16},
    {2, 15, "dgh", p_dgh},
    {2, 16, "rng", p_rng},
};

int main(int argc, char** argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    bool all = argc > 1 && !strcmp(argv[1], "--all");

    uint64_t hwcap  = getauxval(AT_HWCAP);
    uint64_t hwcap2 = getauxval(AT_HWCAP2);
    printf("AT_HWCAP = 0x%lx\nAT_HWCAP2 = 0x%lx\n", hwcap, hwcap2);
    printf("allowlist = 0x%lx 0x%lx\n", (uint64_t)ALLOWED_HWCAP, (uint64_t)ALLOWED_HWCAP2);

    struct sigaction sa = {.sa_handler = sigill_handler};
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGILL, &sa, NULL) < 0) {
        perror("sigaction");
        return 1;
    }

    volatile int failures = 0;
    if ((hwcap & (B(0) | B(1))) != (B(0) | B(1))) {
        printf("FAIL: fp/asimd not advertised (no CPU feature words from the LibOS?)\n");
        failures++;
    }
    if (hwcap & ~(uint64_t)ALLOWED_HWCAP) {
        printf("FAIL: AT_HWCAP bits outside the allowlist: 0x%lx\n", hwcap & ~ALLOWED_HWCAP);
        failures++;
    }
    if (hwcap2 & ~(uint64_t)ALLOWED_HWCAP2) {
        printf("FAIL: AT_HWCAP2 bits outside the allowlist: 0x%lx\n", hwcap2 & ~ALLOWED_HWCAP2);
        failures++;
    }

    volatile int probed = 0;
    for (size_t i = 0; i < sizeof(g_features) / sizeof(g_features[0]); i++) {
        const struct feature* f = &g_features[i];
        uint64_t word = f->word == 1 ? hwcap : hwcap2;
        bool advertised = word & B(f->bit);
        if (all && f->probe) {
            g_in_probe = 1;
            if (sigsetjmp(g_jmp, 1) == 0) {
                bool ok = f->probe();
                g_in_probe = 0;
                printf("%s %s: %s\n", advertised ? "advertised" : "not advertised", f->name,
                       ok ? "executes" : "executes, wrong result");
            } else {
                g_in_probe = 0;
                printf("%s %s: SIGILL\n", advertised ? "advertised" : "not advertised", f->name);
            }
            continue;
        }
        if (!advertised) {
            printf("not advertised: %s\n", f->name);
            continue;
        }
        if (!f->probe) {
            printf("advertised: %s (no instruction probe)\n", f->name);
            continue;
        }
        g_in_probe = 1;
        if (sigsetjmp(g_jmp, 1) == 0) {
            bool ok = f->probe();
            g_in_probe = 0;
            printf("%s: %s\n", ok ? "ok" : "FAIL (wrong result)", f->name);
            if (!ok)
                failures++;
            probed++;
        } else {
            g_in_probe = 0;
            printf("FAIL (SIGILL): %s\n", f->name);
            failures++;
        }
    }

    if (all)
        return 0;
    printf("%d features probed, %d failures\n", probed, failures);
    if (failures)
        return 1;
    printf("TEST OK\n");
    return 0;
}
