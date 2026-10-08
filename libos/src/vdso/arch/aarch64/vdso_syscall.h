/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2020 Intel Corporation
 *                    Borys Popławski <borysp@invisiblethingslab.com>
 */
#pragma once

#include "gramine_entry_api.h"

#ifdef ASAN
#error This code should be compiled without AddressSanitizer.
#endif

#ifdef VDSO_USE_SYSCALL_INSTR
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
#else
static inline long vdso_arch_syscall(long nr, long arg1, long arg2) {
    register long x0 __asm__("x0") = arg1;
    register long x1 __asm__("x1") = arg2;
    register long x8 __asm__("x8") = nr;
    __asm__ volatile(
        "GRAMINE_SYSCALL\n"
        : "+r"(x0)
        : "r"(x1), "r"(x8)
        : "x16", "x30", "memory"
    );
    return x0;
}
#endif
