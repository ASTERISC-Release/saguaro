/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */
/* Copyright (C) 2022 Intel Corporation
 */

/*
 * aarch64 has no CPUID instruction/leaf scheme. Feature discovery here reads AT_HWCAP/AT_HWCAP2
 * from /proc/self/auxv (PAL's own auxv parsing at process startup, in pal_main.c, only looks at
 * AT_PAGESZ/AT_SYSINFO_EHDR and doesn't keep the rest around, so this re-reads it independently
 * rather than threading a new field through generic PAL state for what is a purely cosmetic
 * /proc/cpuinfo feature) and maps the bits onto the same lowercase names Linux's own
 * arch/arm64/kernel/cpuinfo.c uses for the "Features:" line.
 */

#include "elf/elf.h"
#include "libos_cpuid.h"
#include "libos_internal.h"
#include "pal.h"

struct hwcap_flag {
    uint64_t bit;
    const char* name;
};

/* Order matches Linux's arch/arm64/kernel/cpuinfo.c hwcap_str[]/hwcap2_str[] tables (and hence
 * the real kernel's own /proc/cpuinfo "Features:" line ordering). */
static const struct hwcap_flag g_hwcap_flags[] = {
    { 1ULL << 0,  "fp" },
    { 1ULL << 1,  "asimd" },
    { 1ULL << 2,  "evtstrm" },
    { 1ULL << 3,  "aes" },
    { 1ULL << 4,  "pmull" },
    { 1ULL << 5,  "sha1" },
    { 1ULL << 6,  "sha2" },
    { 1ULL << 7,  "crc32" },
    { 1ULL << 8,  "atomics" },
    { 1ULL << 9,  "fphp" },
    { 1ULL << 10, "asimdhp" },
    { 1ULL << 11, "cpuid" },
    { 1ULL << 12, "asimdrdm" },
    { 1ULL << 13, "jscvt" },
    { 1ULL << 14, "fcma" },
    { 1ULL << 15, "lrcpc" },
    { 1ULL << 16, "dcpop" },
    { 1ULL << 17, "sha3" },
    { 1ULL << 18, "sm3" },
    { 1ULL << 19, "sm4" },
    { 1ULL << 20, "asimddp" },
    { 1ULL << 21, "sha512" },
    { 1ULL << 22, "sve" },
    { 1ULL << 23, "asimdfhm" },
    { 1ULL << 24, "dit" },
    { 1ULL << 25, "uscat" },
    { 1ULL << 26, "ilrcpc" },
    { 1ULL << 27, "flagm" },
    { 1ULL << 28, "ssbs" },
    { 1ULL << 29, "sb" },
    { 1ULL << 30, "paca" },
    { 1ULL << 31, "pacg" },
};

static const struct hwcap_flag g_hwcap2_flags[] = {
    { 1ULL << 0,  "dcpodp" },
    { 1ULL << 1,  "sve2" },
    { 1ULL << 2,  "sveaes" },
    { 1ULL << 3,  "svepmull" },
    { 1ULL << 4,  "svebitperm" },
    { 1ULL << 5,  "svesha3" },
    { 1ULL << 6,  "svesm4" },
    { 1ULL << 7,  "flagm2" },
    { 1ULL << 8,  "frint" },
    { 1ULL << 9,  "svei8mm" },
    { 1ULL << 10, "svef32mm" },
    { 1ULL << 11, "svef64mm" },
    { 1ULL << 12, "svebf16" },
    { 1ULL << 13, "i8mm" },
    { 1ULL << 14, "bf16" },
    { 1ULL << 15, "dgh" },
    { 1ULL << 16, "rng" },
    { 1ULL << 17, "bti" },
    { 1ULL << 18, "mte" },
    { 1ULL << 19, "ecv" },
    { 1ULL << 20, "afp" },
    { 1ULL << 21, "rpres" },
    { 1ULL << 22, "mte3" },
};

#define AT_HWCAP  16
#define AT_HWCAP2 26

/* Reads AT_HWCAP/AT_HWCAP2 out of /proc/self/auxv. Falls back to the universally-guaranteed
 * baseline (FP + ASIMD, bits 0 and 1 of HWCAP) if the read fails for any reason -- this is a
 * cosmetic /proc/cpuinfo feature, not worth failing process startup over. */
static void read_hwcaps(uint64_t* out_hwcap, uint64_t* out_hwcap2) {
    *out_hwcap = (1ULL << 0) | (1ULL << 1); /* fp, asimd: always present on aarch64 */
    *out_hwcap2 = 0;

    PAL_HANDLE handle = NULL;
    /* PAL_CREATE_NEVER, not PAL_CREATE_IGNORED: the linux PAL's regular-file open treats IGNORED
     * as "caller should not have used me" (BUG()), and "open it only because it already exists"
     * is exactly the semantics wanted here anyway (the fallback below covers failure). */
    int ret = PalStreamOpen("file:/proc/self/auxv", PAL_ACCESS_RDONLY, /*share_flags=*/0,
                            PAL_CREATE_NEVER, /*options=*/0, &handle);
    if (ret < 0)
        return;

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
            *out_hwcap = auxv[i].a_un.a_val;
        else if (auxv[i].a_type == AT_HWCAP2)
            *out_hwcap2 = auxv[i].a_un.a_val;
    }
}

int libos_get_cpu_flags(char** out_cpu_flags) {
    uint64_t hwcap, hwcap2;
    read_hwcaps(&hwcap, &hwcap2);

    /* Conservative upper bound: every flag name plus a separator, for both tables. */
    size_t max_len = 1;
    for (size_t i = 0; i < ARRAY_SIZE(g_hwcap_flags); i++)
        max_len += strlen(g_hwcap_flags[i].name) + 1;
    for (size_t i = 0; i < ARRAY_SIZE(g_hwcap2_flags); i++)
        max_len += strlen(g_hwcap2_flags[i].name) + 1;

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
    for (size_t i = 0; i < ARRAY_SIZE(g_hwcap2_flags); i++) {
        if (!(hwcap2 & g_hwcap2_flags[i].bit))
            continue;
        if (!first)
            buf[pos++] = ' ';
        first = false;
        size_t len = strlen(g_hwcap2_flags[i].name);
        memcpy(buf + pos, g_hwcap2_flags[i].name, len);
        pos += len;
    }
    buf[pos] = '\0';

    *out_cpu_flags = buf;
    return 0;
}
