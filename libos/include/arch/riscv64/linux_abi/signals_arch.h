/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */
/* Copyright (C) 2023 Intel Corporation
 *                    Michał Kowalczyk <mkow@invisiblethingslab.com>
 */

#pragma once

/* Types and structures used by various Linux ABIs (e.g. syscalls). */
/* These need to be binary-identical with the ones used by Linux. */

#define SIGS_CNT 64
#define SIGRTMIN 32

typedef struct {
    unsigned long __val[SIGS_CNT / (8 * sizeof(unsigned long))];
} __sigset_t;

/* RISC-V has no red zone (unlike x86_64 SysV's 128-byte one below %rsp) -- there is no reserved
 * margin below SP that a signal/interrupt could clobber without the compiler knowing. The psABI
 * additionally mandates 16-byte SP alignment at all times (see the clone stack adjustment in
 * pal/src/host/linux-common/arch/riscv64/syscall.S). */
#define RED_ZONE_SIZE 0
