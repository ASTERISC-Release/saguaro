/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/*
 * riscv64 CPU information for the PAL: fills `struct pal_cpu_info` (the x86-shaped identity
 * record generic PAL/LibOS code passes around; see its definition in arch/riscv64/pal_arch.h).
 *
 * riscv64 has no CPUID instruction, and unlike aarch64 -- whose Main ID Register (MIDR_EL1) is
 * readable at EL0, which is what pal/src/arch/aarch64/pal_cpuid.c decodes -- the RISC-V identity
 * registers (`mvendorid`/`marchid`/`mimpid`) are *supervisor-mode* CSRs with no EL0 read at all.
 * The kernel reads them on our behalf and renders them, per-hart, in /proc/cpuinfo; that file is
 * therefore the only EL0-visible source of CPU identity on this architecture, and this function
 * parses the host's copy of it. Measured format (Ubuntu 24.04 guest kernel 7.0, QEMU `virt`):
 *
 *     processor : 0
 *     hart      : 0
 *     isa       : rv64imafdch_zicbom_..._zbb_zbc_zbs_sstc_svadu
 *     mmu       : sv48
 *     mvendorid : 0x0
 *     marchid   : 0x0
 *     mimpid    : 0x0
 *     hart isa  : <same as isa>
 *
 * with no `uarch` line (older riscv kernels printed one; honored when present) and -- unlike
 * arm64 -- no `BogoMIPS` line at all, so `_PalGetBogomips()` (shared linux-common code) returns
 * 0.0 here by design rather than by failure, and the riscv64 /proc/cpuinfo the LibOS renders for
 * applications must not print a Bogomips line either (see libos/src/arch/riscv64/fs_proc/info.c).
 *
 * Only the *first* hart's block is consulted: heterogeneous SoCs exist (SiFive, Spacemit), and
 * Gramine's contract with the application is one coherent CPU description, so the boot hart's
 * answer is what every vCPU reports -- the same choice the kernel itself makes for cpu0.
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "pal_internal.h"
#include "topo_info.h" /* read_file_buffer() */

#define CPUINFO_BUF_SIZE 4096
#define ISA_STR_SIZE     512
#define MMU_STR_SIZE     32

/* Static storage for the two strings `cpu_brand`/`cpu_mmu` point into: filled once at PAL init
 * (PalCpuInfo is called from pal_main() before any application code runs) and never freed; no
 * malloc is needed, and the pointers stay valid for the lifetime of the process. */
static char g_cpu_isa[ISA_STR_SIZE]         = "unknown";
static char g_cpu_mmu[MMU_STR_SIZE]         = "unknown";
static char g_cpu_uarch[MMU_STR_SIZE * 2]   = "RISC-V";

/* Value of the `word:` entry of the *first* line that *starts* with `word` (the riscv
 * /proc/cpuinfo has both `isa` and `hart isa`, so a bare strstr() would be ambiguous depending
 * on block order), or NULL. Modifies nothing; returns a pointer into `buf`. */
static char* cpuinfo_line_value(char* buf, const char* word) {
    size_t word_len = strlen(word);
    char* p = buf;
    while (p) {
        if (!strncmp(p, word, word_len)) {
            char* q = p + word_len;
            while (*q == ' ' || *q == '\t') {
                q++;
            }
            if (*q == ':') { /* prefix match but not a `word:` field (e.g. "hart isa") is skipped */
                q++;
                while (*q == ' ' || *q == '\t') {
                    q++;
                }
                return q;
            }
        }
        p = strchr(p, '\n');
        if (p) {
            p++; /* next line start */
        }
    }
    return NULL;
}

/* Copy the value of `word` (up to end-of-line) into `out`. Returns false if absent; `out` is
 * left untouched in that case so callers can keep a fallback. */
static bool cpuinfo_copy_entry(char* buf, const char* word, char* out, size_t out_size) {
    char* v = cpuinfo_line_value(buf, word);
    if (!v) {
        return false;
    }
    char* eol = strchr(v, '\n');
    size_t len = eol ? (size_t)(eol - v) : strlen(v);
    if (len >= out_size) {
        len = out_size - 1;
    }
    memcpy(out, v, len);
    out[len] = '\0';
    return true;
}

/*
 * Hide the vector extension from the ISA string an application sees (PLAN.md's "handle F/D only
 * and hide V from applications" decision, the /proc/cpuinfo half of it -- the AT_HWCAP half is
 * libos_get_cpu_flags()). Dropping only the *letter* would leave the zvl/zve-prefixed tokens asserting
 * vector support, so the filter removes every vector-implying token: the compact-form `v` letter,
 * and the underscore-separated multi-letter tokens starting with `v` (e.g. v1p0), zvl, zve,
 * zvv or zvp. In-place: the filtered string is never longer than the input.
 */
static void mask_vector_extensions(char* isa) {
    char*dst = isa;
    char* src = isa;

    /* The first token is the base string ("rv64imafdch" / "rv64gc"): a compact-form vector
     * extension is the single letter 'v' somewhere in it (per the riscv private spec's canonical
     * ordering, 'v' comes after the mandatory letters and before any '_' multi-letter tokens).
     * The scan must skip the "rv64"/"rv32" prefix itself: its 'v' is part of the architecture
     * *name*, not an extension letter (masking it produced the comical "r64imafdc"). */
    size_t base_prefix = (strncmp(src, "rv64", 4) == 0 || strncmp(src, "rv32", 4) == 0) ? 4 : 0;
    size_t idx = 0;
    while (*src && *src != '_') {
        if (*src != 'v' || idx < base_prefix) {
            *dst++ = *src;
        }
        src++;
        idx++;
    }

    /* Remaining underscore-separated multi-letter extension tokens. */
    while (*src == '_') {
        char* tok = src + 1;
        char* next = strchr(tok, '_');
        size_t tok_len = next ? (size_t)(next - tok) : strlen(tok);
        bool vector = (tok_len >= 1 && tok[0] == 'v') ||
                      (tok_len >= 3 && !strncmp(tok, "zvl", 3)) ||
                      (tok_len >= 3 && !strncmp(tok, "zve", 3)) ||
                      (tok_len >= 3 && !strncmp(tok, "zvv", 3)) ||
                      (tok_len >= 3 && !strncmp(tok, "zvp", 3));
        if (!vector) {
            *dst++ = '_';
            memcpy(dst, tok, tok_len);
            dst += tok_len;
        }
        if (!next) {
            break;
        }
        src = next;
    }

    /* A trailing '_' may have been copied along with a token whose *following* vector token was
     * dropped; strip it. (`rv64imafdc_` -> `rv64imafdc`.) */
    if (dst > isa && dst[-1] == '_') {
        dst--;
    }
    *dst = '\0';
}

int _PalGetCPUInfo(struct pal_cpu_info* info) {
    char buf[CPUINFO_BUF_SIZE];
    ssize_t len = read_file_buffer("/proc/cpuinfo", buf, sizeof(buf) - 1);
    if (len < 0) {
        log_warning("Cannot read the host's /proc/cpuinfo (%s); reporting the generic "
                    "\"RISC-V\" fallback identity to the application",
                    unix_strerror(len));
    } else {
        buf[len] = '\0';
        /* First hart block only (see the file comment): cut at the blank line that ends it. */
        char* blank = strstr(buf, "\n\n");
        if (blank) {
            *blank = '\0';
        }

        char isa[ISA_STR_SIZE];
        if (cpuinfo_copy_entry(buf, "isa", isa, sizeof(isa))) {
            mask_vector_extensions(isa);
            snprintf(g_cpu_isa, sizeof(g_cpu_isa), "%s", isa);
        } else {
            log_warning("Host /proc/cpuinfo has no `isa` line; the application will see "
                        "\"unknown\"");
        }

        char mmu[MMU_STR_SIZE];
        if (cpuinfo_copy_entry(buf, "mmu", mmu, sizeof(mmu))) {
            snprintf(g_cpu_mmu, sizeof(g_cpu_mmu), "%s", mmu);
        }

        /* Older riscv kernels rendered the Device Tree `uarch` property ("sifive,u74-mc" and
         * friends); this kernel prints none, so the vendor stays the ISA-level name. */
        char uarch[sizeof(g_cpu_uarch)];
        if (cpuinfo_copy_entry(buf, "uarch", uarch, sizeof(uarch))) {
            snprintf(g_cpu_uarch, sizeof(g_cpu_uarch), "%s", uarch);
        }

        /* The privilege-spec identity triplet, as the kernel read it from the (EL0-invisible)
         * CSRs. All zero on QEMU `virt` (its DT CPU nodes carry no vendor info) -- then the
         * application sees the same zeros the host kernel itself would have reported. */
        char* v;
        if ((v = cpuinfo_line_value(buf, "mvendorid"))) {
            info->cpu_family = (uint64_t)strtol(v, NULL, 0);
        }
        if ((v = cpuinfo_line_value(buf, "marchid"))) {
            info->cpu_model = (uint64_t)strtol(v, NULL, 0);
        }
        if ((v = cpuinfo_line_value(buf, "mimpid"))) {
            info->cpu_stepping = (uint64_t)strtol(v, NULL, 0);
        }
    }

    info->cpu_vendor = g_cpu_uarch;
    info->cpu_brand  = g_cpu_isa;
    info->cpu_mmu    = g_cpu_mmu;

    /* No BogoMIPS: the riscv64 kernel does not compute or print one (unlike arm64's), so
     * `_PalGetBogomips()` finds nothing and yields 0.0 -- and the riscv64-flavored
     * /proc/cpuinfo rendering has no Bogomips line to put it in, exactly like the host's. */
    info->cpu_bogomips = _PalGetBogomips();

    log_debug("CPU identity: vendor `%s` isa `%s` mmu `%s` (mvendorid 0x%lx marchid 0x%lx "
              "mimpid 0x%lx)", info->cpu_vendor, info->cpu_brand, info->cpu_mmu,
              (unsigned long)info->cpu_family, (unsigned long)info->cpu_model,
              (unsigned long)info->cpu_stepping);
    return 0;
}
