/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/*
 * A PAL-level test program for the VBS-enclave PAL (phase 1). It runs INSIDE the enclave on top of
 * the PAL's primitives (pal_vbs_env) and prints its result through the host console OCALL. It is
 * deliberately independent of the Gramine LibOS: phase 1's exit criterion is that the PAL boundary
 * works, not that the LibOS runs (that is phase 2).
 *
 * Each step returns true on success; the suite returns 0 on full success, else the 1-based index of
 * the first failing step.
 */

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "pal_vbs_enclave.h"
#include "packed/packed_abi.h"

static const pal_vbs_env_t* g_env;

static void out(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    if (n > (int)sizeof(buf))
        n = (int)sizeof(buf);
    g_env->console_write(buf, (uint64_t)n);
}

/* A trivial "System V / AAPCS64" callee: it clobbers x18 (via an asm statement) to prove the
 * boundary thunk restores it. It just sums its four arguments. */
static uint64_t sysv_callee(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {
#if defined(__aarch64__)
    __asm__ __volatile__("mov x18, #0xBAD" ::: "x18"); /* a Linux-style clobber of the TEB reg */
#endif
    return a0 + a1 + a2 + a3;
}

static int step_console(void) {
    out("[selftest] step 1: console OCALL works (you are reading this through the host)\n");
    return 1;
}

static int step_memory(void) {
    const uint64_t sz = 1u << 20; /* 1 MiB */
    volatile uint8_t* p = (volatile uint8_t*)g_env->mem_alloc(sz);
    if (!p) {
        out("[selftest] step 2: mem_alloc(1 MiB) FAILED\n");
        return 0;
    }
    for (uint64_t i = 0; i < sz; i += 4096)
        p[i] = (uint8_t)(i >> 12);
    int ok = 1;
    for (uint64_t i = 0; i < sz; i += 4096)
        if (p[i] != (uint8_t)(i >> 12))
            ok = 0;
    g_env->mem_free((void*)p, sz);
    out("[selftest] step 2: RW memory alloc/write/readback %s (1 MiB)\n", ok ? "ok" : "MISMATCH");
    return ok;
}

static int step_time(void) {
    uint64_t t0 = g_env->time_ns();
    uint64_t t1 = g_env->time_ns();
    /* t1 >= t0 and both plausibly after 2020-01-01 (1577836800 s) */
    int ok = (t1 >= t0) && (t0 > 1577836800ull * 1000000000ull);
    out("[selftest] step 3: host time OCALL t0=%llu ns (%s)\n",
        (unsigned long long)t0, ok ? "ok" : "IMPLAUSIBLE");
    return ok;
}

static int step_random(void) {
    uint8_t a[32] = {0}, b[32] = {0};
    if (g_env->random(a, sizeof(a)) != 0 || g_env->random(b, sizeof(b)) != 0) {
        out("[selftest] step 4: BCryptGenRandom FAILED\n");
        return 0;
    }
    int allzero = 1, equal = 1;
    for (int i = 0; i < 32; i++) {
        if (a[i])
            allzero = 0;
        if (a[i] != b[i])
            equal = 0;
    }
    int ok = !allzero && !equal;
    out("[selftest] step 4: random %s (first bytes %02x %02x)\n", ok ? "ok" : "SUSPECT", a[0], a[1]);
    return ok;
}

static int step_abi_x18(void) {
    uint64_t before = g_env->read_x18();
    uint64_t sum = g_env->enter_sysv((void*)sysv_callee, 1, 2, 3, 4);
    uint64_t after = g_env->read_x18();
    int ok = (sum == 10) && (before == after) && (before != 0);
    out("[selftest] step 5: enter_sysv sum=%llu x18 %s (0x%llx -> 0x%llx)\n",
        (unsigned long long)sum, (before == after) ? "preserved" : "CLOBBERED",
        (unsigned long long)before, (unsigned long long)after);
    return ok;
}

static int step_seh(void) {
    int64_t r = g_env->try_brk_resume();
    int ok = (r == 0);
    out("[selftest] step 6: SEH caught brk and resumed (r=%lld) %s\n",
        (long long)r, ok ? "ok" : "FAILED");
    return ok;
}

/* ---- step 7: packed objects (phase 2) ---- */

/* the PAL table handed to the packed object: write() goes to the host console */
static int64_t packed_write(int64_t fd, const char* buf, int64_t len) {
    (void)fd;
    if (len < 0)
        return -1;
    g_env->console_write(buf, (uint64_t)len);
    return len;
}

/* bound by the loader to the packed object's undefined symbol of the same name */
static int64_t import_triple(int64_t x) {
    return 3 * x;
}

static int run_packed(const char* name) {
    static const struct pal_vbs_import imports[] = {
        { "pal_vbs_import_triple", (void*)import_triple },
    };
    struct pal_vbs_loaded obj;
    uint64_t detail = 0;
    int64_t ret = g_env->packed_load(name, imports, sizeof(imports) / sizeof(imports[0]), &obj,
                                     &detail);
    if (ret < 0) {
        out("[selftest] step 7: load %s FAILED: %lld (detail 0x%llx)\n", name, (long long)ret,
            (unsigned long long)detail);
        return 0;
    }
    out("[selftest] step 7: %s at %p size 0x%llx: %llu relocations, %llu imports bound, "
        "%llu RELRO pages read-only\n", name, (void*)obj.base, (unsigned long long)obj.size,
        (unsigned long long)obj.nrela, (unsigned long long)obj.nimports,
        (unsigned long long)obj.nrelro);
    void* sym = g_env->packed_sym(&obj, "packed_entry");
    if (!obj.entry || sym != obj.entry) {
        out("[selftest] step 7: entry %p, symbol lookup %p: MISMATCH\n", obj.entry, sym);
        return 0;
    }
    struct pal_vbs_packed_table table = { .write = packed_write };
    uint64_t x18_before = g_env->read_x18();
    int64_t r = (int64_t)g_env->enter_sysv(obj.entry, (uint64_t)(uintptr_t)&table, 0, 0, 0);
    uint64_t x18_after = g_env->read_x18();
    /* the message is 59 bytes; see packed_test.c */
    int64_t want = PAL_VBS_PACKED_EXPECT(59);
    int ok = (r == want) && (x18_before == x18_after);
    out("[selftest] step 7: %s packed_entry returned %lld, expected %lld, x18 %s: %s\n", name,
        (long long)r, (long long)want, x18_before == x18_after ? "preserved" : "CLOBBERED",
        ok ? "ok" : "FAILED");
    return ok;
}

static int step_packed(void) {
    uint64_t n = g_env->packed_count(), ntest = 0;
    const char* first = NULL;
    int ok = 1;
    for (uint64_t i = 0; i < n; i++) {
        const char* name = g_env->packed_name(i);
        if (strncmp(name, "libpacked", 9))
            continue; /* the boot objects (libpal, LibOS, application) are not test objects */
        ntest++;
        first = first ? first : name;
        ok &= run_packed(name);
    }
    out("[selftest] step 7: %llu packed objects in the image, %llu test objects\n",
        (unsigned long long)n, (unsigned long long)ntest);
    if (!ntest) {
        out("[selftest] step 7: no packed test objects in this image: skipped\n");
        return 1;
    }
    /* refusals: unknown name, and preparing the same object twice */
    struct pal_vbs_loaded obj;
    int64_t r1 = g_env->packed_load("no-such-object", NULL, 0, &obj, NULL);
    int64_t r2 = g_env->packed_load(first, NULL, 0, &obj, NULL);
    int refused = (r1 == PAL_VBS_PK_NOTFOUND) && (r2 == PAL_VBS_PK_LOADED);
    out("[selftest] step 7: refusals (unknown %lld, twice %lld) %s\n", (long long)r1,
        (long long)r2, refused ? "ok" : "FAILED");
    return ok && refused;
}

/* ---- step 8: does a non-TEB value in x18 survive preemption? ---- */
static int step_x18_preempt(void) {
    const uint64_t magic = 0x5a5a18185a5a1818ull;
    const uint64_t iterations = 4000000000ull; /* about 1.3 s at 3 GHz, one iteration per cycle */
    uint64_t t0 = g_env->time_ns();
    uint64_t seen = g_env->x18_spin(iterations, magic);
    uint64_t t1 = g_env->time_ns();
    /* A measurement of the platform, not a check of the PAL: on Windows 11 25H2 the kernel reloads
     * x18 with the TEB when a preempted enclave thread resumes (FINDINGS.md, 2026-10-15), which is
     * why every packed object must be built with -ffixed-x18. Either outcome passes. */
    uint64_t teb = g_env->read_x18();
    out("[selftest] step 8: x18 held 0x%llx for %llu ms of spinning: %s (seen 0x%llx)\n",
        (unsigned long long)magic, (unsigned long long)((t1 - t0) / 1000000),
        seen == magic ? "preserved" : seen == teb ? "reset to the TEB" : "CHANGED to another value",
        (unsigned long long)seen);
    return 1;
}

/* ---- step 11: which registers survive preemption (TPIDR_EL0, GPRs, NEON)? measurement ---- */
static int step_regs_preempt(void) {
    uint64_t tp = 0;
    uint64_t t0 = g_env->time_ns();
    uint64_t mask = g_env->regs_spin(4000000000ull, &tp);
    uint64_t t1 = g_env->time_ns();
    out("[selftest] step 11: %llu ms of spinning: changed-register mask 0x%016llx, TPIDR_EL0 "
        "afterwards 0x%llx (%s)\n", (unsigned long long)((t1 - t0) / 1000000),
        (unsigned long long)mask, (unsigned long long)tp,
        mask ? "SOME REGISTERS CHANGED" : "all preserved");
    return 1; /* a measurement */
}

/* ---- step 12: does TPIDR_EL0 survive OCALLs (enclave -> host -> enclave)? measurement ---- */
static int step_tpidr_ocall(void) {
    uint64_t saved = g_env->get_tpidr();
    const uint64_t magic = 0x7ee2000000000000ull;
    uint64_t changed = 0, first_seen = 0, n = 20000;
    uint64_t t0 = g_env->time_ns();
    for (uint64_t i = 0; i < n; i++) {
        g_env->set_tpidr(magic + i);
        (void)g_env->time_ns(); /* one OCALL */
        uint64_t v = g_env->get_tpidr();
        if (v != magic + i) {
            if (!changed)
                first_seen = v;
            changed++;
        }
    }
    uint64_t t1 = g_env->time_ns();
    g_env->set_tpidr(saved);
    out("[selftest] step 12: TPIDR_EL0 across %llu OCALLs (%llu ms): changed %llu times%s",
        (unsigned long long)n, (unsigned long long)((t1 - t0) / 1000000),
        (unsigned long long)changed, changed ? ", first value seen " : "\n");
    if (changed)
        out("0x%llx\n", (unsigned long long)first_seen);
    return 1; /* a measurement */
}

/* ---- steps 9 and 10: SP outside the thread's Windows stack (the LibOS's and the application's
 * stacks are pool memory) ---- */
static int step_foreign_stack_spin(void) {
    out("[selftest] step 9: spinning ~1.3 s with SP in VirtualAlloc'd memory...\n");
    uint64_t t0 = g_env->time_ns();
    int64_t r = g_env->spin_foreign_stack(4000000000ull);
    uint64_t t1 = g_env->time_ns();
    out("[selftest] step 9: survived %llu ms of preemptions with a foreign SP (r=%lld)\n",
        (unsigned long long)((t1 - t0) / 1000000), (long long)r);
    return r == 0;
}

static int step_foreign_stack_brk(void) {
    /* the probe whose unwind data names a handler: first with the TEB bounds widened to the stack
     * in use, then without; a failure ends the process (the exception cannot be dispatched) */
    out("[selftest] step 10a: brk on a foreign stack, own SEH handler, TEB bounds widened...\n");
    int64_t r1 = g_env->brk_probe(1);
    out("[selftest] step 10a: %s (r=0x%llx)\n", r1 == 0 ? "handled" : "FAILED", (unsigned long long)r1);
#ifdef PAL_VBS_SELFTEST_10B
    /* ends the process on Windows 11 25H2 (FINDINGS.md): run only on purpose */
    out("[selftest] step 10b: the same without widening the TEB bounds...\n");
    int64_t r2 = g_env->brk_probe(0);
    out("[selftest] step 10b: %s (r=0x%llx)\n", r2 == 0 ? "handled" : "FAILED", (unsigned long long)r2);
    return r1 == 0 && r2 == 0;
#else
    return r1 == 0;
#endif
}

int64_t pal_vbs_selftest(const pal_vbs_env_t* env, uint64_t* run, uint64_t* passed) {
    g_env = env;
    int (*steps[])(void) = {
        step_console, step_memory, step_time, step_random, step_abi_x18, step_seh, step_packed,
        step_x18_preempt, step_regs_preempt, step_tpidr_ocall, step_foreign_stack_spin, step_foreign_stack_brk,
    };
    uint64_t n = sizeof(steps) / sizeof(steps[0]);
    *run = 0;
    *passed = 0;
    int64_t first_fail = 0;
    out("[selftest] VBS PAL self-test: %llu steps\n", (unsigned long long)n);
    for (uint64_t i = 0; i < n; i++) {
        (*run)++;
        if (steps[i]()) {
            (*passed)++;
        } else if (!first_fail) {
            first_fail = (int64_t)(i + 1);
        }
    }
    out("[selftest] done: %llu/%llu passed, result=%lld\n",
        (unsigned long long)*passed, (unsigned long long)*run, (long long)first_fail);
    return first_fail;
}
