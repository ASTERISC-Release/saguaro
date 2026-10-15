/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/*
 * AT_HWCAP / AT_HWCAP2 for the aarch64 vm PAL: there is no host kernel between the application
 * and the CPU, so the words are computed from the ID registers, which the PAL reads at EL1 and
 * which KVM (or QEMU's TCG) already sanitises for the guest. The field/minimum-value pairs are
 * those of Linux's arm64_elf_hwcaps[] (arch/arm64/kernel/cpufeature.c), for the bits of the
 * allowlist in pal_hwcap.h only (pal_cpuid.c masks again). EVTSTRM and CPUID are never set: this
 * kernel neither enables the timer event stream nor emulates EL0 reads of the ID registers.
 * See Documentation/devel/aarch64/hwcap/PLAN.md.
 */

#include <stdint.h>

#include "pal_hwcap.h"
#include "pal_internal.h"

/* generic encodings, so that the assembler needs no knowledge of the register names */
#define READ_SYSREG(name) ({                              \
    uint64_t __v;                                         \
    __asm__ volatile("mrs %0, " name : "=r"(__v));        \
    __v;                                                  \
})
#define ID_AA64PFR0_EL1  "s3_0_c0_c4_0"
#define ID_AA64ISAR0_EL1 "s3_0_c0_c6_0"
#define ID_AA64ISAR1_EL1 "s3_0_c0_c6_1"
#define ID_AA64MMFR2_EL1 "s3_0_c0_c7_2"

static unsigned int field(uint64_t reg, unsigned int shift) {
    return (reg >> shift) & 0xf;
}

/* signed 4-bit field (FP, AdvSIMD): 0xf means "not implemented" */
static int sfield(uint64_t reg, unsigned int shift) {
    int v = (int)field(reg, shift);
    return v >= 8 ? v - 16 : v;
}

void _PalGetHwcaps(uint64_t* out_hwcap, uint64_t* out_hwcap2) {
    uint64_t pfr0  = READ_SYSREG(ID_AA64PFR0_EL1);
    uint64_t isar0 = READ_SYSREG(ID_AA64ISAR0_EL1);
    uint64_t isar1 = READ_SYSREG(ID_AA64ISAR1_EL1);
    uint64_t mmfr2 = READ_SYSREG(ID_AA64MMFR2_EL1);

    uint64_t hwcap = 0, hwcap2 = 0;

    /* ID_AA64PFR0_EL1 */
    if (sfield(pfr0, 16) >= 0) hwcap |= PAL_HWCAP_FP;
    if (sfield(pfr0, 16) >= 1) hwcap |= PAL_HWCAP_FPHP;
    if (sfield(pfr0, 20) >= 0) hwcap |= PAL_HWCAP_ASIMD;
    if (sfield(pfr0, 20) >= 1) hwcap |= PAL_HWCAP_ASIMDHP;

    /* ID_AA64ISAR0_EL1 */
    if (field(isar0, 4) >= 1)  hwcap |= PAL_HWCAP_AES;
    if (field(isar0, 4) >= 2)  hwcap |= PAL_HWCAP_PMULL;
    if (field(isar0, 8) >= 1)  hwcap |= PAL_HWCAP_SHA1;
    if (field(isar0, 12) >= 1) hwcap |= PAL_HWCAP_SHA2;
    if (field(isar0, 12) >= 2) hwcap |= PAL_HWCAP_SHA512;
    if (field(isar0, 16) >= 1) hwcap |= PAL_HWCAP_CRC32;
    if (field(isar0, 20) >= 2) hwcap |= PAL_HWCAP_ATOMICS;
    if (field(isar0, 28) >= 1) hwcap |= PAL_HWCAP_ASIMDRDM;
    if (field(isar0, 32) >= 1) hwcap |= PAL_HWCAP_SHA3;
    if (field(isar0, 36) >= 1) hwcap |= PAL_HWCAP_SM3;
    if (field(isar0, 40) >= 1) hwcap |= PAL_HWCAP_SM4;
    if (field(isar0, 44) >= 1) hwcap |= PAL_HWCAP_ASIMDDP;
    if (field(isar0, 48) >= 1) hwcap |= PAL_HWCAP_ASIMDFHM;
    if (field(isar0, 52) >= 1) hwcap |= PAL_HWCAP_FLAGM;
    if (field(isar0, 52) >= 2) hwcap2 |= PAL_HWCAP2_FLAGM2;
    if (field(isar0, 60) >= 1) hwcap2 |= PAL_HWCAP2_RNG;

    /* ID_AA64ISAR1_EL1 */
    if (field(isar1, 0) >= 1)  hwcap |= PAL_HWCAP_DCPOP;
    if (field(isar1, 0) >= 2)  hwcap2 |= PAL_HWCAP2_DCPODP;
    if (field(isar1, 12) >= 1) hwcap |= PAL_HWCAP_JSCVT;
    if (field(isar1, 16) >= 1) hwcap |= PAL_HWCAP_FCMA;
    if (field(isar1, 20) >= 1) hwcap |= PAL_HWCAP_LRCPC;
    if (field(isar1, 20) >= 2) hwcap |= PAL_HWCAP_ILRCPC;
    if (field(isar1, 32) >= 1) hwcap2 |= PAL_HWCAP2_FRINT;
    if (field(isar1, 36) >= 1) hwcap |= PAL_HWCAP_SB;
    if (field(isar1, 44) >= 1) hwcap2 |= PAL_HWCAP2_BF16;
    if (field(isar1, 48) >= 1) hwcap2 |= PAL_HWCAP2_DGH;
    if (field(isar1, 52) >= 1) hwcap2 |= PAL_HWCAP2_I8MM;

    /* ID_AA64MMFR2_EL1 */
    if (field(mmfr2, 32) >= 1) hwcap |= PAL_HWCAP_USCAT;

    log_debug("ID_AA64PFR0_EL1 0x%lx ISAR0 0x%lx ISAR1 0x%lx MMFR2 0x%lx", pfr0, isar0, isar1,
              mmfr2);
    *out_hwcap  = hwcap;
    *out_hwcap2 = hwcap2;
}
