/* SPDX-License-Identifier: LGPL-3.0-or-later */

/*
 * pal_start-aarch64.S
 *
 * This file contains architecture-specific implementation of pal_start.
 */

.text
.global pal_start
.type pal_start, %function
.align 4
pal_start:
    .cfi_startproc
    mov  x0, sp   /* 1st arg for pal_linux_main: initial SP */
    /* 2nd arg: fini callback. On x86_64 this is read from %rdx, mirroring the SysV ABI
     * convention where the dynamic linker passes its termination function that way; aarch64 has
     * no defined analog of that convention, and pal_linux_main() does not currently use
     * fini_callback at all (see its TODO), so we simply pass through whatever is in x1 at entry
     * rather than inventing a register convention that nothing relies on. */
    mov  x29, #0  /* mark the last stack frame with FP == 0 (for debuggers) */
    mov  x30, #0  /* and LR == 0 (no meaningful return address) */
    mov  x2, sp
    and  x2, x2, #0xfffffffffffffff0
    mov  sp, x2
    bl   pal_linux_main
    /* Unreachable. */
    .cfi_endproc

.section .note.GNU-stack,"",%progbits
