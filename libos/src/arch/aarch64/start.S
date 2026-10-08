/* Our PAL loader invokes this entrypoint with all registers unspecified, except SP.
 * For detailed description of the stack layout see pal/src/pal_rtld.c:start_execution().
 */

    .text
    .globl libos_start
    .type libos_start, %function
libos_start:
    .cfi_startproc

    /* Clear the frame pointer. The ABI suggests this be done, to mark the outermost frame
     * obviously. */
    mov  x29, #0

    /* Arguments for libos_init(argv, envp): */
    ldr  x8, [sp]              /* argc, unused by libos_init, used to calculate envp's location */
    add  x0, sp, #8            /* argv */
    add  x1, x0, x8, lsl #3
    add  x1, x1, #8            /* envp, after all args (including argv[argc] == NULL) */

    /* Required by AAPCS64. */
    mov  x9, sp
    and  x9, x9, #0xfffffffffffffff0
    mov  sp, x9

    adrp x9, :got:libos_init
    ldr  x9, [x9, :got_lo12:libos_init]
    blr  x9

    .cfi_endproc

.section .note.GNU-stack,"",%progbits
