/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */
/* Copyright (C) 2022 Intel Corporation
 */

/*!
 * \brief Get CPU feature flags.
 *
 * \param[out] out_cpu_flags  On success, set to CPU feature flags.
 *
 * \returns 0 on success, negative on failure.
 *
 * This function returns a new buffer with CPU feature flags, the caller is responsible to free it
 * afterwards.
 *
 * riscv64 has no CPUID-leaf flag bitmap the way x86_64 does; instead this reads AT_HWCAP/
 * AT_HWCAP2 from /proc/self/auxv (see libos_cpuid.c) and maps the COMPAT_*-free single-word
 * feature bits (the `zbb`/`zba`/`v`-style extension bits of riscv_hwcap) onto the same lowercase
 * flag names Linux's own riscv64 /proc/cpuinfo "isa" line spells. The vector bit must be masked
 * out of anything this reports for now (Documentation/devel/riscv/PLAN.md decisions: handle F/D
 * only and hide V from applications); the test guest's ISA has no V anyway.
 */
int libos_get_cpu_flags(char** out_cpu_flags);
