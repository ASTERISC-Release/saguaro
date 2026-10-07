/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Constant-time buffer comparison (aarch64 port of the x86_64 ct_memequal.nasm) */

.text
.global ct_memequal
.type ct_memequal, %function

/* bool ct_memequal(const void* lhs, const void* rhs, size_t count)
 * Arguments:
 * - X0: pointer to the first buffer (lhs)
 * - X1: pointer to the second buffer (rhs)
 * - X2: number of bytes to compare (count)
 */
ct_memequal:
    mov     w3, #0          // accumulator
    cbz     x2, .Lreturn
    add     x2, x2, x0      // x2 = end = lhs + count
.Lloop:
    ldrb    w4, [x0], #1
    ldrb    w5, [x1], #1
    eor     w4, w4, w5      // bitwise, not a branch -- keeps this constant-time
    orr     w3, w3, w4
    cmp     x0, x2
    b.ne    .Lloop
.Lreturn:
    cmp     w3, #0
    cset    w0, eq
    ret

.section .note.GNU-stack,"",%progbits
