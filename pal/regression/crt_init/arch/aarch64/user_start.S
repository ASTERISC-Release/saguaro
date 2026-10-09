/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */
/* Copyright (C) 2022 Intel Corporation
 *                    Borys Popławski <borysp@invisiblethingslab.com>
 */

/* GAS port of user_start.nasm -- NASM has no aarch64 backend.
 *
 * Our PAL loader invokes this entrypoint with all registers unspecified, except SP.
 * For detailed description of the stack layout see pal/src/pal_rtld.c:start_execution().
 *
 * All four external symbols below are referenced via the GOT (`:got:`/`:got_lo12:`), same as
 * libos/src/arch/aarch64/start.S's libos_init call: safe regardless of whether this binary ends
 * up PIE or not, and regardless of whether a given symbol turns out to be same-object or
 * cross-object, which is one less thing to get wrong by hand here.
 */

.text
.global _start
.type _start, %function
_start:
    /* Clear the frame pointer. The ABI suggests this be done, to mark the outermost frame. */
    mov  x29, #0

    /* Arguments for `pal_regression_start_main`: */
    ldr  x8, [sp]            /* argc */
    add  x1, sp, #8          /* argv */
    add  x2, x1, x8, lsl #3
    add  x2, x2, #8          /* envp, after all args (including argv[argc] == NULL) */
    adrp x3, :got:main
    ldr  x3, [x3, :got_lo12:main] /* &main */
    mov  x0, x8              /* argc */

    /* Required by AAPCS64. */
    mov  x9, sp
    and  x9, x9, #0xfffffffffffffff0
    mov  sp, x9

    adrp x9, :got:pal_regression_start_main
    ldr  x9, [x9, :got_lo12:pal_regression_start_main]
    blr  x9

    /* pal_regression_start_main's return value is already in x0, where PalProcessExit expects
     * its argument. */
    adrp x9, :got:PalProcessExit
    ldr  x9, [x9, :got_lo12:PalProcessExit]
    blr  x9

.section .note.GNU-stack,"",%progbits
