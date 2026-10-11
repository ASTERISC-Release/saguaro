/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

#pragma once

#include "libos_types.h"

int __vdso_clock_gettime(clockid_t clock, struct timespec* t);
int __vdso_gettimeofday(struct timeval* tv, struct timezone* tz);
long __vdso_getcpu(unsigned* cpu, void* unused_cache);
/* No __vdso_time() here: riscv64's generic syscall ABI has no __NR_time at all (asm-generic
 * table, exactly as on aarch64) -- real riscv64 glibc/musl implement time() via
 * clock_gettime(CLOCK_REALTIME, ...) instead of a dedicated vDSO symbol, and we don't export one
 * either. */
