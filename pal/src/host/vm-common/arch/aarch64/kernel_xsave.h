/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2023 Intel Corporation */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/*
 * aarch64 counterpart of vm-common/kernel_xsave.h.
 *
 * Deliberately keeps the x86 spellings (`xsave`, `XSAVE area`, `g_xsave_size`) for the public
 * API, because the shared C (kernel_thread.c, kernel_sched.c) speaks in terms of "a per-thread
 * FP save area of `g_xsave_size` bytes at `VM_XSAVE_ALIGN` alignment, initialized to
 * `g_xsave_reset_state`". On aarch64 that area holds a PAL_XREGS_STATE (32 128-bit V registers
 * plus FPCR/FPSR), saved/restored with `stp q..`-style accesses driven by FPCR reads instead of
 * the `xsave`/`xrstor` instructions; see kernel_fpsimd.c.
 */

#pragma once

#include <stdint.h>

#define VM_XSAVE_ALIGN 16

/* The reset state is just an all-zero FP area (V regs zero, FPCR/FPSR zero -- FPCR==0 means
 * round-to-nearest, no flush-to-zero, which is also x86's reset state). */
#define VM_XSAVE_RESET_STATE_SIZE (sizeof(uint64_t))

extern uint32_t g_xsave_size;
extern const uint64_t g_xsave_reset_state[VM_XSAVE_RESET_STATE_SIZE / sizeof(uint64_t)];

/* Enables FP/SIMD access at EL1 and EL0 (CPACR_EL1.FPEN = 0b11); the x86 `xsave_init()` analog. */
int xsave_init(void);

/* Save/restore the FP area; call these only when the current thread's `fpregs` buffer is the
 * one to operate on (same contract as the x86 `do_xsave`/`do_xrstor` asm helpers). */
void do_xsave(void* fpsimd_area);
void do_xrstor(void* fpsimd_area);
