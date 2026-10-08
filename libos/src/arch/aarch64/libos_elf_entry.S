/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2022 Intel Corporation
 *                    Mariusz Zaborski <oshogbo@invisiblethingslab.com> */

/* GAS port of libos_elf_entry.nasm -- NASM has no aarch64 backend.
 *
 * Before jumping to the entry point, we set:
 * - X2: zeroed, for symmetry with x86_64's RDX=0 "no atexit function" convention (see the
 *   ambiguity noted in pal_start.S's comment: aarch64 has no well-defined equivalent of this
 *   convention to begin with, so this is defensive, not load-bearing).
 * - SP: the initial stack, contains program arguments and environment.
 * - PSTATE.NZCV: zeroed (AArch64's equivalent of clearing the low FLAGS bits).
 * - FPCR: zeroed. Unlike x86_64's MXCSR (whose architectural reset value traps on every FP
 *   exception, hence needing an explicit 0x1F80 to mask them), AArch64's FPCR reset value of 0
 *   already means "exceptions set FPSR status flags but never trap" -- the behavior x86_64 has
 *   to ask for explicitly is simply FPCR's default here, so there's no equivalent of the magic
 *   0x1F80 constant to encode.
 */

.text
.global call_elf_entry
.type call_elf_entry, %function

/* noreturn void call_elf_entry(elf_addr_t entry, void* argp) */
call_elf_entry:
    mov  x2, #0
    msr  fpcr, x2
    msr  nzcv, x2
    mov  sp, x1   /* set stack pointer to second arg */
    br   x0       /* jump to entry point (first arg) */

.section .note.GNU-stack,"",%progbits
