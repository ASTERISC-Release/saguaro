/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/*
 * pal_start-riscv64.S
 *
 * This file contains architecture-specific implementation of pal_start.
 */

.text
.global pal_start
.type pal_start, %function
.align 4
pal_start:
    .cfi_startproc
    mv   a0, sp   /* 1st arg for pal_linux_main: initial SP */
    /* 2nd arg: fini callback. On x86_64 this is read from %rdx, mirroring the SysV ABI
     * convention where the dynamic linker passes its termination function that way; riscv64 has
     * no defined analog of that convention (the kernel enters a no-INTERP ET_DYN with only sp
     * defined), and pal_linux_main() does not currently use fini_callback at all (see its TODO),
     * so we simply pass through whatever is in a1 at entry rather than inventing a register
     * convention that nothing relies on. */
    li   fp, 0    /* mark the last stack frame with FP(s0) == 0 (for debuggers) */
    li   ra, 0    /* and LR == 0 (no meaningful return address; `call` reloads it anyway) */
    /* The RISC-V psABI mandates 16-byte stack alignment; the kernel already entered us with a
     * 16-aligned sp (execve enforces it), the mask below is just belt-and-braces parity with the
     * aarch64 version. */
    andi sp, sp, -16
    call pal_linux_main
    /* Unreachable. */
    .cfi_endproc

.section .note.GNU-stack,"",%progbits
