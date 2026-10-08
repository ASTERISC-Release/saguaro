/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2023 Intel Corporation
 *                    Michał Kowalczyk <mkow@invisiblethingslab.com>
 */

#pragma once

/* Types and structures used by various Linux ABIs (e.g. syscalls). */
/* These need to be binary-identical with the ones used by Linux. */

#include <linux/mman.h>

/* MAP_FIXED_NOREPLACE and MAP_SHARED_VALIDATE are fairly new and might not be defined. */
#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif // MAP_FIXED_NOREPLACE
#ifndef MAP_SHARED_VALIDATE
#define MAP_SHARED_VALIDATE 0x03
#endif // MAP_SHARED_VALIDATE

#ifndef MADV_FREE
#define MADV_FREE 8
#endif
/* madvise() flags are part of the generic Linux UAPI, not an arch-specific syscall table -- these
 * fallback values are the same on aarch64 (verified against a real aarch64 <linux/mman.h>), so,
 * unlike the historical `#ifdef __x86_64__` guard here implied, there's no actual platform
 * dependency to gate on. */
#ifndef MADV_WIPEONFORK
#define MADV_WIPEONFORK 18
#endif
#ifndef MADV_KEEPONFORK
#define MADV_KEEPONFORK 19
#endif
