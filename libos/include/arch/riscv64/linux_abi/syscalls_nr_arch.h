/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */
/* Copyright (C) 2020 Intel Corporation
 *                    Michał Kowalczyk <mkow@invisiblethingslab.com>
 */

#pragma once

#include <asm/unistd.h>

/* riscv64 uses the same generic syscall ABI (asm-generic/unistd.h) as aarch64: the numbers come
 * straight from the toolchain's own <asm/unistd.h> (verified up through __NR_riscv_hwprobe == 258
 * and the rest of the table on this box's headers). Per the port plan (decisions table in
 * dev docs: riscv/PLAN.md) the dispatch table is *shared* with aarch64 rather than
 * copied: riscv64 adds only two calls of its own (below), has the same omissions as aarch64 (no
 * vfork/open/pipe/getdents/... legacy calls -- see libos_table.c for how each maps onto the
 * *at()/generic replacement Gramine already implements), and where the two architectures' headers
 * both define a number, they define the same one. If a future build machine's headers are older,
 * the build fails with a clear "undeclared identifier" rather than silently using a wrong number
 * -- add a fallback here if/when that happens.
 *
 * Two RISC-V-specific calls exist beyond the shared generic set; they are listed here explicitly
 * so libos_table.c can reference them unconditionally (the *host* kernel may be older than the
 * headers -- at runtime these return ENOSYS on such kernels, which the LibOS forwards as-is): */
#ifndef __NR_riscv_hwprobe
#define __NR_riscv_hwprobe 258
#endif
#ifndef __NR_riscv_flush_icache
#define __NR_riscv_flush_icache 259
#endif

/* Graphics, syscall no. for acquiring lock from gramine to synchronize libgraphics comms.
 * Picked comfortably above the highest real riscv64 syscall number so it can never collide with
 * a future kernel addition; mirrors __NR_ACQUIRE_LLOCK/__NR_RELINQUISH_LLOCK in libos_table.c,
 * which are defined directly there (same as the x86_64/aarch64 ports) rather than here. */
#ifndef __NR_register_host_identity_fd
#define __NR_register_host_identity_fd 470
#endif

/* Like on aarch64, the real header on this box *does* already define __NR_syscalls (to one past
 * the highest real syscall) -- so `#ifndef` here would silently keep that too-small value instead
 * of making room for our custom syscall numbers up to 501. Override it unconditionally. */
#undef __NR_syscalls
#define __NR_syscalls 550
