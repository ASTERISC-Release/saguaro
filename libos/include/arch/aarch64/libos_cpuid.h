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
 * aarch64 has no CPUID-leaf flag bitmap the way x86_64 does; instead this reads AT_HWCAP/
 * AT_HWCAP2 from /proc/self/auxv (see libos_cpuid.c) and maps the bits onto the same lowercase
 * flag names ("fp", "asimd", "aes", ...) Linux's own /proc/cpuinfo "Features:" line uses.
 */
int libos_get_cpu_flags(char** out_cpu_flags);
