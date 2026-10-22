/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */
/* Copyright (C) 2020 Intel Corporation
 *                    Borys Popławski <borysp@invisiblethingslab.com>
 */
#pragma once

#include "saguaro_entry_api.h"

#ifdef ASAN
#error This code should be compiled without AddressSanitizer.
#endif

/*
 * Unlike x86_64's version of this file, this always issues a raw `svc #0` regardless of
 * VDSO_USE_SYSCALL_INSTR: x86_64's "fast path" jumps through `%gs:GRAMINE_SYSCALL_OFFSET`, pure
 * CPU register state that needs no linking at all. aarch64's GRAMINE_SYSCALL macro (see
 * saguaro_entry_api.h) instead references the exported `g_pal_tcb_singleton` symbol -- which
 * does not exist from this translation unit's point of view, because vdso.so is built as its
 * own tiny, dependency-free shared object (`-Wl,--no-undefined`, no link against libpal.so,
 * exactly like a real vDSO) that this macro structurally cannot reach into. Falling back to a
 * real `svc` here is not a loss for Milestone A specifically: with no libc patched to use
 * GRAMINE_SYSCALL in the first place (see the Milestone A plan notes), every syscall already
 * goes through the seccomp-trap path (maybe_emulate_syscall() in libos_context.c), which is
 * exactly what a raw `svc` here will hit too -- so this is the *consistent* choice, not a
 * workaround bolted on top of a broken fast path.
 */
static inline long vdso_arch_syscall(long nr, long arg1, long arg2) {
    register long x0 __asm__("x0") = arg1;
    register long x1 __asm__("x1") = arg2;
    register long x8 __asm__("x8") = nr;
    __asm__ volatile(
        "svc #0\n"
        : "+r"(x0)
        : "r"(x1), "r"(x8)
        : "memory"
    );
    return x0;
}
