/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */
/* Copyright (C) 2022 Intel Corporation
 */

/*
 * riscv64 CPU feature flags (port of the aarch64 libos_cpuid.c).
 *
 * There is no CPUID-leaf scheme and, unlike AArch64, no exported `hwcap_str` table: the uapi
 * <asm/hwcap.h> defines AT_HWCAP bits only for the single-letter extensions, as
 * `COMPAT_HWCAP_ISA_<letter> == (1 << ('<letter>' - 'A'))`. Newer kernels do stuff additional
 * (multi-letter) extensions into AT_HWCAP through an internal `riscv_isa_ext` mapping that is
 * deliberately not part of the header and is unstable across kernel versions -- so we decode
 * only the single-letter bits here and never guess the rest. This is lossless in practice:
 * the full, authoritative extension list is the /proc/cpuinfo `isa` line, which Gramine
 * renders verbatim (vector-masked) from the host kernel's own cpuinfo (see
 * pal/src/arch/riscv64/pal_cpuid.c).
 *
 * The vector bit must be masked out of anything reported (dev docs: riscv/PLAN.md
 * decisions: F/D only, V hidden from applications; the PAL side masks V from the `isa` string
 * identically, and the riscv_v_ext state in the signal-context ABI is never parsed or
 * synthesized). It is simply absent from the table below, which is the most direct way to
 * guarantee no consumer can ever see it.
 *
 * Measured on the riscv64 test guest (QEMU `virt`, kernel 7.0.0-generic):
 * AT_HWCAP == 0x112d == bits a(0) c(2) d(3) f(5) i(8) m(12); no AT_HWCAP2.
 */

#include "elf/elf.h"
#include "libos_cpuid.h"
#include "libos_internal.h"
#include "pal.h"

struct hwcap_flag {
    uint64_t bit;
    const char* name;
};

/* Bit positions per uapi <asm/hwcap.h> `COMPAT_HWCAP_ISA_*`; names spelled the same lowercase
 * way the riscv64 /proc/cpuinfo `isa` token spells them. `v` intentionally absent. */
static const struct hwcap_flag g_hwcap_flags[] = {
    { 1ULL << ('A' - 'A'), "a" },
    { 1ULL << ('C' - 'A'), "c" },
    { 1ULL << ('D' - 'A'), "d" },
    { 1ULL << ('F' - 'A'), "f" },
    { 1ULL << ('I' - 'A'), "i" },
    { 1ULL << ('M' - 'A'), "m" },
};

#define AT_HWCAP 16

/* Reads AT_HWCAP out of /proc/self/auxv. Falls back to the rv64gc baseline (i m a f d c --
 * Ubuntu riscv64's whole userspace is built for it and Gramine's own manifests mount that
 * userspace) if the read fails for any reason: this feeds a cosmetic /proc/cpuinfo feature,
 * not worth failing process startup over. */
static uint64_t read_hwcap(void) {
    uint64_t hwcap = (1ULL << ('I' - 'A')) | (1ULL << ('M' - 'A')) | (1ULL << ('A' - 'A')) |
                     (1ULL << ('F' - 'A')) | (1ULL << ('D' - 'A')) | (1ULL << ('C' - 'A'));

    PAL_HANDLE handle = NULL;
    /* PAL_CREATE_NEVER, not PAL_CREATE_IGNORED: the linux PAL's regular-file open treats IGNORED
     * as "caller should not have used me" (BUG()), and "open it only because it already exists"
     * is exactly the semantics wanted here anyway (the fallback above covers failure). */
    int ret = PalStreamOpen("file:/proc/self/auxv", PAL_ACCESS_RDONLY, /*share_flags=*/0,
                            PAL_CREATE_NEVER, /*options=*/0, &handle);
    if (ret < 0)
        return hwcap;

    elf_auxv_t auxv[64];
    size_t total = 0;
    while (total < sizeof(auxv)) {
        size_t count = sizeof(auxv) - total;
        ret = PalStreamRead(handle, /*offset=*/0, &count, (char*)auxv + total);
        if (ret < 0 || count == 0)
            break;
        total += count;
    }
    PalObjectDestroy(handle);

    for (size_t i = 0; i * sizeof(auxv[0]) + sizeof(auxv[0]) <= total; i++) {
        if (auxv[i].a_type == AT_HWCAP)
            hwcap = auxv[i].a_un.a_val;
    }
    return hwcap;
}

int libos_get_cpu_flags(char** out_cpu_flags) {
    uint64_t hwcap = read_hwcap();

    /* Conservative upper bound: every flag name plus a separator. */
    size_t max_len = 1;
    for (size_t i = 0; i < ARRAY_SIZE(g_hwcap_flags); i++)
        max_len += strlen(g_hwcap_flags[i].name) + 1;

    char* buf = malloc(max_len);
    if (!buf)
        return -PAL_ERROR_NOMEM;

    size_t pos = 0;
    bool first = true;
    for (size_t i = 0; i < ARRAY_SIZE(g_hwcap_flags); i++) {
        if (!(hwcap & g_hwcap_flags[i].bit))
            continue;
        if (!first)
            buf[pos++] = ' ';
        first = false;
        size_t len = strlen(g_hwcap_flags[i].name);
        memcpy(buf + pos, g_hwcap_flags[i].name, len);
        pos += len;
    }
    buf[pos] = '\0';

    *out_cpu_flags = buf;
    return 0;
}
