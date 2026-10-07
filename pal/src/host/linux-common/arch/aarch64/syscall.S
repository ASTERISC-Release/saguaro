/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2021 Intel Corporation
 *                    Borys Popławski <borysp@invisiblethingslab.com>
 */

#define __ASSEMBLY__
#include <asm/unistd.h>

/* Not pulled from <linux/sched.h>/<asm/signal.h> to avoid dragging non-assembler-safe headers
 * into this file; these three values are long-stable ABI constants. */
#define CLONE_VM    0x00000100
#define CLONE_VFORK 0x00004000
#define SIGCHLD     17

.global gramine_raw_syscalls_code_begin
.global gramine_raw_syscalls_code_end
.type gramine_raw_syscalls_code_begin, %function
.type gramine_raw_syscalls_code_end, %function
gramine_raw_syscalls_code_begin:

.global do_syscall
.type do_syscall, %function
.align 4
do_syscall:
    .cfi_startproc
    /* long do_syscall(long nr, long a1, a2, a3, a4, a5, a6)
     * AAPCS64 already places nr in x0 and a1..a6 in x1..x6 (8 integer argument registers means
     * no stack spill is needed here, unlike x86_64 which only has 6 and must pull a6 off the
     * stack) -- the only shuffle needed is moving the syscall number from x0 to x8, since
     * aarch64 keeps it separate from the argument registers (x86_64's `syscall` instead reuses
     * the same register, rax, for both the number and the return value). */
    mov  x8, x0
    mov  x0, x1
    mov  x1, x2
    mov  x2, x3
    mov  x3, x4
    mov  x4, x5
    mov  x5, x6
    svc  #0
    ret
    .cfi_endproc

.global clone
.type clone, %function
.align 4
/* C declaration (AAPCS64 has 8 integer argument registers, so -- unlike x86_64, which spills the
 * last two to the stack -- every argument fits in a register):
 * long clone(int (*f)(void*),          // x0
 *            void* stack,              // x1
 *            int flags,                // x2
 *            void* arg,                // x3
 *            void* parent_tid,         // x4
 *            void* tls,                // x5
 *            void* child_tid,          // x6
 *            void (*exit_func)(int)    // x7
 * );
 */
clone:
    .cfi_startproc
    /* Pass f/arg/exit_func on the child stack, same layout as the x86_64 version. */
    sub  x1, x1, #0x18
    stp  x0, x3, [x1]       /* [x1+0x00] = f, [x1+0x08] = arg */
    str  x7, [x1, #0x10]    /* [x1+0x10] = exit_func */

    mov  x9, x2             /* flags (x0-x4 are about to be overwritten for the syscall) */
    mov  x10, x4            /* parent_tid */
    mov  x11, x6            /* child_tid */
    mov  x12, x5            /* tls */

    mov  x0, x9             /* flags */
    /* x1 already holds the (adjusted) child stack pointer */
    mov  x2, x10            /* parent_tid */
    mov  x3, x11            /* child_tid */
    mov  x4, x12            /* tls */
    mov  x8, #__NR_clone
    svc  #0

    cbz  x0, .Lchild
    ret

.Lchild:
    .cfi_undefined x30
    .cfi_undefined x29
    ldr  x9, [sp]           /* x9 = f */
    ldr  x0, [sp, #0x8]     /* x0 = arg (1st/only param for f) */
    ldr  x10, [sp, #0x10]   /* x10 = exit_func */
    add  sp, sp, #0x18      /* restore sp to the original `stack` value */
    blr  x9                 /* call f(arg); return value lands in x0 */
    blr  x10                 /* call exit_func(x0) -- x0 is already positioned as its 1st arg */
    /* Unreachable. */
    brk  #0
    .cfi_endproc

.global vfork
.type vfork, %function
.align 4
vfork:
    .cfi_startproc
    /* __NR_vfork does not exist on aarch64's syscall ABI; emulate via clone() with newsp == 0,
     * which tells the kernel to let the child keep running on the parent's current stack --
     * exactly vfork's defining semantics. */
    mov  x9, x30             /* save the return address across the syscall */
    .cfi_register x30, x9
    mov  x0, #(CLONE_VM | CLONE_VFORK | SIGCHLD)
    mov  x1, #0              /* newsp = 0: child keeps parent's SP */
    mov  x2, #0              /* parent_tid */
    mov  x3, #0              /* child_tid */
    mov  x4, #0              /* tls */
    mov  x8, #__NR_clone
    svc  #0
    br   x9
    .cfi_endproc

.global syscall_rt_sigreturn
.type syscall_rt_sigreturn, %function
.align 4
syscall_rt_sigreturn:
    .cfi_startproc
    mov  x8, #__NR_rt_sigreturn
    svc  #0
    .cfi_endproc

.global _PalThreadExit_asm_stub
.type _PalThreadExit_asm_stub, %function
_PalThreadExit_asm_stub:
    .cfi_startproc
    str  wzr, [x0]              /* spinlock_unlock(&g_thread_stack_lock) */
    cbz  x1, .Lskip_clear_child_tid /* check if clear_child_tid != NULL */
    str  wzr, [x1]               /* set *clear_child_tid = 0 */
.Lskip_clear_child_tid:
    mov  x0, #0
    mov  x8, #__NR_exit
    svc  #0
    brk  #0
    b    .Lskip_clear_child_tid
    .cfi_endproc

gramine_raw_syscalls_code_end:

.section .note.GNU-stack,"",%progbits
