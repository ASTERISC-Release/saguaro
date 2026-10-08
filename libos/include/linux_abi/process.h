/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2023 Intel Corporation
 *                    Michał Kowalczyk <mkow@invisiblethingslab.com>
 */

#pragma once

/* Types and structures used by various Linux ABIs (e.g. syscalls). */
/* These need to be binary-identical with the ones used by Linux. */

#ifdef __x86_64__
/* ARCH_SET_FS/ARCH_GET_FS etc. -- arch_prctl() doesn't exist on aarch64 at all; TLS is set
 * directly via TPIDR_EL0 (see libos_tcb_arch.h), no syscall or header involved. */
#include <asm/prctl.h>
#endif
#include <linux/sched.h> // for CLONE_* stuff
#include <linux/wait.h>
