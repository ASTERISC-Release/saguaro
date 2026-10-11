/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */
/* Constant-time buffer comparison (riscv64 port of the x86_64 ct_memequal.nasm) */

.text
.global ct_memequal
.type ct_memequal, %function

/* bool ct_memequal(const void* lhs, const void* rhs, size_t count)
 * Arguments:
 * - a0: pointer to the first buffer (lhs)
 * - a1: pointer to the second buffer (rhs)
 * - a2: number of bytes to compare (count)
 */
ct_memequal:
    li      t0, 0           // accumulator
    beqz    a2, .Lreturn
    add     t1, a0, a2      // t1 = end = lhs + count
.Lloop:
    lbu     t2, 0(a0)
    lbu     t3, 0(a1)
    addi    a0, a0, 1
    addi    a1, a1, 1
    xor     t2, t2, t3      // bitwise, not a branch -- keeps this constant-time
    or      t0, t0, t2
    bne     a0, t1, .Lloop
.Lreturn:
    seqz    a0, t0          // RISC-V has no flags; seqz materializes (t0 == 0) into a0
    ret

.section .note.GNU-stack,"",%progbits
