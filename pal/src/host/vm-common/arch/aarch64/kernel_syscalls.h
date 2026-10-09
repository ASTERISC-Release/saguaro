/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2023 Intel Corporation */

/*
 * aarch64 counterpart of vm-common/kernel_syscalls.h: the vector table replaces STAR/LSTAR/
 * SYSCALL_MASK. SYSCALL_RT_SIGRETURN keeps the x86 file's role (special return path out of
 * rt_sigreturn) with the Linux aarch64 syscall number (139, see asm/unistd.h); the return is
 * done by writing ELR/SPSR/SP_EL0 and `eret` instead of pushing an iretq frame.
 */

#pragma once

/* rt_sigreturn has a special return-from-syscall path, see kernel_context.S */
#define SYSCALL_RT_SIGRETURN 139

int syscalls_init(void);
