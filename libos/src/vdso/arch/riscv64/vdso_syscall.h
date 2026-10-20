/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */
/* Copyright (C) 2020 Intel Corporation
 *                    Borys Popławski <borysp@invisiblethingslab.com>
 */
#pragma once

#include "gramine_entry_api.h"

#ifdef ASAN
#error This code should be compiled without AddressSanitizer.
#endif

/*
 * Like the aarch64 sibling file, this always issues a raw `ecall` regardless of
 * VDSO_USE_SYSCALL_INSTR: x86_64's "fast path" jumps through `%gs:GRAMINE_SYSCALL_OFFSET`, pure
 * CPU register state that needs no linking at all. riscv64's GRAMINE_SYSCALL macro (see
 * gramine_entry_api.h) instead references the exported `g_pal_tcb_singleton` symbol -- which
 * does not exist from this translation unit's point of view, because vdso.so is built as its
 * own tiny, dependency-free shared object (`-Wl,--no-undefined`, no link against libpal.so,
 * exactly like a real vDSO) that this macro structurally cannot reach into. Falling back to a
 * real `ecall` here is not a loss: with no libc patched to use GRAMINE_SYSCALL in the first
 * place (riscv64 decision: seccomp-trap only, see dev docs: riscv/PLAN.md), every
 * syscall already goes through the seccomp-trap path (maybe_emulate_syscall() in
 * libos_context.c), which is exactly what a raw `ecall` here will hit too -- so this is the
 * *consistent* choice, not a workaround bolted on top of a broken fast path.
 *
 * Unlike x86_64 (rcx/r11 clobbered) and matching aarch64, `ecall` destroys no registers; the
 * "+r"(a0) output plus the two inputs are the full truth about register liveness.
 */
static inline long vdso_arch_syscall(long nr, long arg1, long arg2) {
    register long a0 __asm__("a0") = arg1;
    register long a1 __asm__("a1") = arg2;
    register long a7 __asm__("a7") = nr;
    __asm__ volatile(
        "ecall\n"
        : "+r"(a0)
        : "r"(a1), "r"(a7)
        : "memory"
    );
    return a0;
}
