/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/*
 * AArch64 AT_HWCAP / AT_HWCAP2 bits (values of Linux's uapi <asm/hwcap.h>) and the allowlist of
 * bits the PALs may pass to applications (Documentation/devel/aarch64/hwcap/PLAN.md).
 *
 * A bit is allowed only if its feature is plain user-space instructions on general registers,
 * FP/SIMD registers V0-V31 and memory: no state beyond what the PALs and the LibOS save (X
 * registers, PSTATE flags, V0-V31, FPCR/FPSR), no system call, no trap handling. SVE/SME (the
 * LibOS signal frame and the vm kernel keep V0-V31 only), PAC (no key management), BTI (glibc
 * then asks for PROT_BTI), MTE (PROT_MTE, tag faults), DIT/SSBS (PSTATE bits across signals not
 * reviewed) and every newer bit stay masked. EVTSTRM and CPUID are allowed, but only the direct
 * PAL reports them (the host kernel provides the event stream and emulates EL0 reads of the ID
 * registers).
 */

#pragma once

#define PAL_HWCAP_FP       (1UL << 0)
#define PAL_HWCAP_ASIMD    (1UL << 1)
#define PAL_HWCAP_EVTSTRM  (1UL << 2)
#define PAL_HWCAP_AES      (1UL << 3)
#define PAL_HWCAP_PMULL    (1UL << 4)
#define PAL_HWCAP_SHA1     (1UL << 5)
#define PAL_HWCAP_SHA2     (1UL << 6)
#define PAL_HWCAP_CRC32    (1UL << 7)
#define PAL_HWCAP_ATOMICS  (1UL << 8)
#define PAL_HWCAP_FPHP     (1UL << 9)
#define PAL_HWCAP_ASIMDHP  (1UL << 10)
#define PAL_HWCAP_CPUID    (1UL << 11)
#define PAL_HWCAP_ASIMDRDM (1UL << 12)
#define PAL_HWCAP_JSCVT    (1UL << 13)
#define PAL_HWCAP_FCMA     (1UL << 14)
#define PAL_HWCAP_LRCPC    (1UL << 15)
#define PAL_HWCAP_DCPOP    (1UL << 16)
#define PAL_HWCAP_SHA3     (1UL << 17)
#define PAL_HWCAP_SM3      (1UL << 18)
#define PAL_HWCAP_SM4      (1UL << 19)
#define PAL_HWCAP_ASIMDDP  (1UL << 20)
#define PAL_HWCAP_SHA512   (1UL << 21)
#define PAL_HWCAP_SVE      (1UL << 22)
#define PAL_HWCAP_ASIMDFHM (1UL << 23)
#define PAL_HWCAP_DIT      (1UL << 24)
#define PAL_HWCAP_USCAT    (1UL << 25)
#define PAL_HWCAP_ILRCPC   (1UL << 26)
#define PAL_HWCAP_FLAGM    (1UL << 27)
#define PAL_HWCAP_SSBS     (1UL << 28)
#define PAL_HWCAP_SB       (1UL << 29)
#define PAL_HWCAP_PACA     (1UL << 30)
#define PAL_HWCAP_PACG     (1UL << 31)

#define PAL_HWCAP2_DCPODP  (1UL << 0)
#define PAL_HWCAP2_FLAGM2  (1UL << 7)
#define PAL_HWCAP2_FRINT   (1UL << 8)
#define PAL_HWCAP2_I8MM    (1UL << 13)
#define PAL_HWCAP2_BF16    (1UL << 14)
#define PAL_HWCAP2_DGH     (1UL << 15)
#define PAL_HWCAP2_RNG     (1UL << 16)

#define PAL_HWCAP_ALLOWED (PAL_HWCAP_FP | PAL_HWCAP_ASIMD | PAL_HWCAP_EVTSTRM | PAL_HWCAP_AES     \
                           | PAL_HWCAP_PMULL | PAL_HWCAP_SHA1 | PAL_HWCAP_SHA2 | PAL_HWCAP_CRC32 \
                           | PAL_HWCAP_ATOMICS | PAL_HWCAP_FPHP | PAL_HWCAP_ASIMDHP              \
                           | PAL_HWCAP_CPUID | PAL_HWCAP_ASIMDRDM | PAL_HWCAP_JSCVT              \
                           | PAL_HWCAP_FCMA | PAL_HWCAP_LRCPC | PAL_HWCAP_DCPOP | PAL_HWCAP_SHA3 \
                           | PAL_HWCAP_SM3 | PAL_HWCAP_SM4 | PAL_HWCAP_ASIMDDP                   \
                           | PAL_HWCAP_SHA512 | PAL_HWCAP_ASIMDFHM | PAL_HWCAP_USCAT             \
                           | PAL_HWCAP_ILRCPC | PAL_HWCAP_FLAGM | PAL_HWCAP_SB)

#define PAL_HWCAP2_ALLOWED (PAL_HWCAP2_DCPODP | PAL_HWCAP2_FLAGM2 | PAL_HWCAP2_FRINT           \
                            | PAL_HWCAP2_I8MM | PAL_HWCAP2_BF16 | PAL_HWCAP2_DGH                \
                            | PAL_HWCAP2_RNG)
