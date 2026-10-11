/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */
/* Copyright (C) 2026 The ASTERISC Research Lab
 */

/* GAS port of start.S (x86_64 nasm / aarch64 GAS) -- riscv64 process entry. */

.text
    .globl libos_start
    .type libos_start, %function
libos_start:
    .cfi_startproc

    /* Clear the frame pointer. The ABI suggests this be done, to mark the outermost frame
     * obviously. */
    li   fp, 0

    /* Arguments for libos_init(argv, envp): */
    ld   t0, 0(sp)          /* argc, unused by libos_init, used to calculate envp's location */
    addi a0, sp, 8          /* argv */
    slli t1, t0, 3
    addi a1, a0, 8
    add  a1, a1, t1         /* envp, after all args (including argv[argc] == NULL) */

    /* Required by the RISC-V psABI: stack pointer must stay 16-byte aligned. The kernel/LibOS
     * entry point gives us an aligned sp already (unlike aarch64 where AAPCS64 mandates the
     * explicit mask); do it anyway for parity and safety. */
    andi sp, sp, -16

    /* `la` of the cross-object symbol resolves through the GOT (riscv64 analog of aarch64's
     * `adrp :got:` pair); libsysdb's GOT is populated by the PAL's own ELF loader. */
    la   t2, libos_init
    jalr t2

    .cfi_endproc

.section .note.GNU-stack,"",%progbits
