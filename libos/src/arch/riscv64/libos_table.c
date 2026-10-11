/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/*
 * riscv64 system call dispatch table (port of the aarch64 libos_table.c).
 *
 * PHASE-1 STUB: the table exists with the correct type and full __NR_syscalls size so the tree
 * links; every entry is NULL (the generic dispatcher in libos_syscalls.c already turns a NULL
 * entry into -ENOSYS). Phase 4 of Documentation/devel/riscv/plan.md populates it.
 *
 * Per the port's decisions the riscv64 table is the *same generic-ABI table* as aarch64's: both
 * architectures take their syscall numbers from asm-generic/unistd.h, so the entries are
 * identical except for (a) none (riscv64 adds __NR_riscv_hwprobe/__NR_riscv_flush_icache, which
 * have no LibOS emulation requirement beyond plain passthrough, and __NR_register_host_identity_fd
 * with the same value as aarch64's) and (b) the x86_64-only legacy syscalls (open/pipe/...) that
 * neither generic-ABI architecture has. The Phase-4 implementation should therefore *share* the
 * table definition between the two arch directories (single source file, both arch dirs), rather
 * than maintain copies -- this placeholder deliberately does not pre-empt that refactor.
 */

#include "libos_internal.h"
#include "libos_table.h"
#include "linux_abi/syscalls_nr_arch.h"

libos_syscall_t libos_syscall_table[LIBOS_SYSCALL_BOUND] = {0};
