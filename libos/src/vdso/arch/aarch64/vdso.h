/* SPDX-License-Identifier: LGPL-3.0-or-later */

#pragma once

#include "libos_types.h"

int __vdso_clock_gettime(clockid_t clock, struct timespec* t);
int __vdso_gettimeofday(struct timeval* tv, struct timezone* tz);
long __vdso_getcpu(unsigned* cpu, void* unused_cache);
/* No __vdso_time() here: aarch64's generic syscall ABI has no __NR_time at all (see
 * libos_table.c) -- real aarch64 glibc/musl implement time() via clock_gettime(CLOCK_REALTIME,
 * ...) instead of a dedicated vDSO symbol, and we don't export one either. */
