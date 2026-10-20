/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/*
 * aarch64 has no CPUID-leaf equivalent of x86_64's vendor/brand/family/model/stepping scheme.
 * Instead we read the Main ID Register (MIDR_EL1), which is directly accessible from EL0, and
 * map its fields onto the generic `pal_cpu_info` struct the same way Linux's own
 * arch/arm64/kernel/cpuinfo.c does for /proc/cpuinfo ("CPU implementer", "CPU architecture",
 * "CPU variant", "CPU part", "CPU revision").
 *
 * The feature words for applications (AT_HWCAP / AT_HWCAP2) come from the host PAL
 * (_PalGetHwcaps(): host auxv, ID registers or the enclave runtime) and are masked here, in one
 * place for every PAL, to the allowlist of pal_hwcap.h.
 */

#include "pal_hwcap.h"
#include "pal_internal.h"

#define MIDR_IMPLEMENTER(midr) (((midr) >> 24) & 0xff)
#define MIDR_VARIANT(midr)     (((midr) >> 20) & 0xf)
#define MIDR_ARCHITECTURE(midr) (((midr) >> 16) & 0xf)
#define MIDR_PARTNUM(midr)     (((midr) >> 4) & 0xfff)
#define MIDR_REVISION(midr)    ((midr) & 0xf)

static uint64_t read_midr_el1(void) {
#ifdef PAL_HOST_VBS
    /* Inside a VBS enclave an ID-register read raises an exception (phase 0, step d of
     * dev docs: vbs-enclave/): report an unknown implementer instead. */
    return 0;
#else
    uint64_t midr;
    __asm__("mrs %0, midr_el1" : "=r"(midr));
    return midr;
#endif
}

static const char* implementer_name(uint32_t implementer) {
    /* Known JEP106 implementer IDs, as listed in Linux's arch/arm64/kernel/cpuinfo.c
     * (arm64_implementers[]); unrecognized values fall back to a hex string below. */
    switch (implementer) {
        case 0x41: return "ARM Limited";
        case 0x42: return "Broadcom Corporation";
        case 0x43: return "Cavium Inc.";
        case 0x44: return "Digital Equipment Corporation";
        case 0x46: return "Fujitsu Ltd.";
        case 0x48: return "HiSilicon Technologies Inc.";
        case 0x49: return "Infineon Technologies AG";
        case 0x4d: return "Motorola/Freescale Semiconductor Inc.";
        case 0x4e: return "NVIDIA Corporation";
        case 0x50: return "Applied Micro Circuits Corporation";
        case 0x51: return "Qualcomm Inc.";
        case 0x53: return "Samsung Electronics Co. Ltd.";
        case 0x56: return "Marvell International Ltd.";
        case 0x61: return "Apple Inc.";
        case 0x66: return "Faraday Technology Corporation";
        case 0x69: return "Intel Corporation";
        case 0xc0: return "Ampere Computing";
        default: return NULL;
    }
}

int _PalGetCPUInfo(struct pal_cpu_info* ci) {
    uint64_t midr = read_midr_el1();
    uint32_t implementer = MIDR_IMPLEMENTER(midr);
    uint32_t variant      = MIDR_VARIANT(midr);
    uint32_t architecture = MIDR_ARCHITECTURE(midr);
    uint32_t partnum      = MIDR_PARTNUM(midr);
    uint32_t revision     = MIDR_REVISION(midr);

    const char* known_vendor = implementer_name(implementer);
    char* vendor_id = malloc(32);
    if (!vendor_id)
        return -PAL_ERROR_NOMEM;
    if (known_vendor) {
        snprintf(vendor_id, 32, "%s", known_vendor);
    } else {
        snprintf(vendor_id, 32, "0x%02x", implementer);
    }
    ci->cpu_vendor = vendor_id;

    char* brand = malloc(64);
    if (!brand) {
        free(vendor_id);
        return -PAL_ERROR_NOMEM;
    }
    snprintf(brand, 64, "midr 0x%016lx (implementer 0x%02x, part 0x%03x)",
             midr, implementer, partnum);
    ci->cpu_brand = brand;

    /* Architecture nibble is 0xf ("see ID_AA64PFR0_EL1") on every real ARMv8-A/ARMv9-A part;
     * Linux's /proc/cpuinfo itself just reports the fixed value 8 for all AArch64 cores
     * (see c_show() in arch/arm64/kernel/cpuinfo.c), so we match that instead of exposing the
     * uninformative raw MIDR architecture field. */
    __UNUSED(architecture);
    ci->cpu_family   = 8;
    ci->cpu_model    = partnum;
    ci->cpu_stepping = (variant << 4) | revision;

    uint64_t hwcap = 0, hwcap2 = 0;
    _PalGetHwcaps(&hwcap, &hwcap2);
    ci->cpu_hwcap  = hwcap & PAL_HWCAP_ALLOWED;
    ci->cpu_hwcap2 = hwcap2 & PAL_HWCAP2_ALLOWED;
    log_debug("AT_HWCAP 0x%lx AT_HWCAP2 0x%lx (from the platform: 0x%lx 0x%lx)", ci->cpu_hwcap,
              ci->cpu_hwcap2, hwcap, hwcap2);

#ifdef PAL_HOST_VM
    /* vm kernel (aarch64): -mgeneral-regs-only, so _PalGetBogomips() returns the IEEE-754
     * bit pattern of the double in a GPR; reinterpret it with an integer copy. */
    uint64_t bogomips_bits = _PalGetBogomips();
    memcpy(&ci->cpu_bogomips, &bogomips_bits, sizeof(ci->cpu_bogomips));
    if (!bogomips_bits) {
        log_warning("bogomips could not be retrieved, passing 0.0 to the application");
    }
#else
    ci->cpu_bogomips = _PalGetBogomips();
    if (ci->cpu_bogomips == 0.0) {
        log_warning("bogomips could not be retrieved, passing 0.0 to the application");
    }
#endif

    return 0;
}
