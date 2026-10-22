/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */
/* Copyright (C) 2022 Intel Corporation
 *                    Borys Popławski <borysp@invisiblethingslab.com>
 */

/* GAS port of user_start.nasm -- NASM has no riscv64 backend.
 *
 * Our PAL loader invokes this entrypoint with all registers unspecified, except SP.
 * For detailed description of the stack layout see pal/src/pal_rtld.c:start_execution().
 *
 * The three external symbols (main, pal_regression_start_main, PalProcessExit) are referenced
 * via `la`, which for a dynamic symbol assembles to the GOT-load pair (riscv64 analog of the
 * aarch64 sibling's explicit `:got:`/`:got_lo12:` references): safe regardless of whether the
 * symbol is same-object or cross-object. These binaries are linked `-no-pie` but still against
 * libpal.so, so the two PAL symbols genuinely need the GOT; `main` resolves directly.
 *
 * Unlike the aarch64 version, this file must also initialize `gp` (x3): the PAL regression
 * binaries are *non-PIE executables*, and the riscv64 linker relaxes references to their
 * small data sections (.sdata/.sdata2 -- GCC places file-scope initialized objects <=8 bytes
 * there) into gp-relative addressing, whose base the linker defines as __global_pointer$ (the
 * start of .sdata plus 0x800). The kernel does not plant `gp` for us at execve of a
 * Saguaro-loaded binary, so -- exactly like glibc's csu/start.S REGISTER_GP macro -- the entry
 * code sets it itself before any relaxed code can run. (`la gp, ...` must be wrapped in
 * `.option norelax` so the relaxation machinery cannot rewrite this very instruction.)
 */

.text
.global _start
.type _start, %function
_start:
    .option push
    .option norelax
    la   gp, __global_pointer$
    .option pop

    /* Clear the frame pointer. The ABI suggests this be done, to mark the outermost frame. */
    li   fp, 0

    /* Arguments for `pal_regression_start_main(argc, argv, envp, main)`: */
    ld   t0, 0(sp)           /* argc */
    mv   a0, t0              /* argc */
    addi a1, sp, 8           /* argv */
    slli t1, t0, 3
    addi a2, a1, 8
    add  a2, a2, t1          /* envp, after all args (including argv[argc] == NULL) */
    la   a3, main            /* &main */

    /* Required by the RISC-V psABI: 16-byte stack alignment. */
    andi sp, sp, -16

    la   t1, pal_regression_start_main
    jalr t1                  /* clobbers ra (x1): outermost frame, nothing to return to */

    /* pal_regression_start_main's return value is already in a0, where PalProcessExit expects
     * its argument. */
    la   t1, PalProcessExit
    jr   t1                  /* tail call */

.section .note.GNU-stack,"",%progbits
