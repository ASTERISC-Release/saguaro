/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */
/* Copyright (C) 2020 Intel Corporation
 *                    Michał Kowalczyk <mkow@invisiblethingslab.com>
 */

#pragma once

#include <asm/unistd.h>

/* Unlike the x86_64 version of this file, we don't carry a long list of `#ifndef` fallback
 * defines for syscalls that might be missing from an older build machine's headers: this port
 * targets a native aarch64 build on a single, current box, not broad cross-distro portability,
 * and this box's headers already define aarch64's full generic syscall ABI (verified up through
 * __NR_lsm_list_modules == 461). If a future build machine's headers are older, the build will
 * fail with a clear "undeclared identifier" rather than silently using a wrong number -- add a
 * guard here if/when that happens.
 *
 * aarch64 uses a completely different (and much smaller/denser) syscall numbering than x86_64's
 * legacy table: there is no 1:1 mapping between the two, and aarch64 has no legacy path-based
 * syscalls at all (no open/stat/lstat/access/poll/select/dup2/fork/vfork/pipe/getdents/alarm/
 * time/creat/link/unlink/symlink/readlink/chmod/chown/lchown/rename/utime/utimes/mkdir/rmdir/
 * mknod/arch_prctl/...) -- see libos_table.c for how each of those maps onto the *at()/generic
 * replacement Gramine already implements. */

/* Graphics, syscall no. for acquiring lock from gramine to synchronize libgraphics comms.
 * Picked comfortably above the highest real aarch64 syscall number (461) so it can never collide
 * with a future kernel addition; mirrors __NR_ACQUIRE_LLOCK/__NR_RELINQUISH_LLOCK in
 * libos_table.c, which are defined directly there (same as the x86_64 port) rather than here. */
#ifndef __NR_register_host_identity_fd
#define __NR_register_host_identity_fd 470
#endif

/* Unlike __NR_register_host_identity_fd above, the real header on this box *does* already define
 * __NR_syscalls (to 462, one past the highest real syscall) -- so `#ifndef` here would silently
 * keep that too-small value instead of making room for our custom syscall numbers up to 501.
 * Override it unconditionally instead. */
#undef __NR_syscalls
#define __NR_syscalls 550
