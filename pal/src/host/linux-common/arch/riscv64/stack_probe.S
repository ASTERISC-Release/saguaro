/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */
/* Copyright (C) 2022 Intel Corporation
 *                    Borys Popławski <borysp@invisiblethingslab.com> */

/* GAS port of stack_probe.nasm -- NASM has no riscv64 backend. */

.global probe_stack
.type probe_stack, %function

/* void probe_stack(size_t pages_count) */
probe_stack:
    /* The x86_64 original (`sub/sp/loop`) and the aarch64 GAS port are do-while loops:
     * `probe_stack(0)` decrements the counter below zero (x86: RCX wraps to 2^64-1) and
     * probes essentially forever -- a latent crash, masked only because every caller in
     * the tree passes a compile-time positive constant (THREAD_STACK_SIZE / PAGE_SIZE).
     * Rather than replicate the latent bug, this port returns immediately for 0; for any
     * count >= 1 the observable behavior (pages touched, sp restored) is identical to the
     * x86_64 original -- see the Phase 2 log entry in dev docs: riscv/PLAN.md. */
    beqz a0, .Lreturn
    mv   t0, sp
    /* addi's immediate reaches only 12 signed bits (+-2048), so the 4096-byte stride goes
     * through a register (AArch64's `sub` takes a 12-bit *unsigned* immediate, hence #0x1000
     * there). */
    li   t1, -0x1000
.Lrep:
    add  sp, sp, t1
    sd   zero, 0(sp)
    addi a0, a0, -1
    bnez a0, .Lrep

.Lreturn:
    mv   sp, t0
    ret

.section .note.GNU-stack,"",%progbits
