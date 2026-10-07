/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2022 Intel Corporation
 *                    Borys Popławski <borysp@invisiblethingslab.com> */

/* GAS port of stack_probe.nasm -- NASM has no aarch64 backend. */

.global probe_stack
.type probe_stack, %function

/* void probe_stack(size_t pages_count) */
probe_stack:
    mov  x1, sp
.Lrep:
    sub  sp, sp, #0x1000
    str  xzr, [sp]
    subs x0, x0, #1
    b.ne .Lrep

    mov  sp, x1
    ret

.section .note.GNU-stack,"",%progbits
