/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */
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
     * RISC-V's calling convention already places nr in a0 and a1..a6 in a1..a6 (8 argument
     * registers means no stack spill is needed here, like aarch64, unlike x86_64 which only has
     * 6 and must pull a6 off the stack) -- the only shuffle needed is moving the syscall number
     * from a0 to a7, since riscv64 keeps it separate from the argument registers (exactly as
     * aarch64 keeps it in x8; x86_64's `syscall` instead reuses the same register, rax, for both
     * the number and the return value). */
    mv  a7, a0
    mv  a0, a1
    mv  a1, a2
    mv  a2, a3
    mv  a3, a4
    mv  a4, a5
    mv  a5, a6
    ecall
    ret
    .cfi_endproc

.global clone
.type clone, %function
.align 4
/* C declaration (RISC-V has 8 argument registers, so -- like aarch64, unlike x86_64, which spills
 * the last two to the stack -- every argument fits in a register):
 * long clone(int (*f)(void*),          // a0
 *            void* stack,              // a1
 *            int flags,                // a2
 *            void* arg,                // a3
 *            void* parent_tid,         // a4
 *            void* tls,                // a5
 *            void* child_tid,          // a6
 *            void (*exit_func)(int)    // a7
 * );
 */
clone:
    .cfi_startproc
    /* Pass f/arg/exit_func on the child stack, same layout as the x86_64/aarch64 versions.
     * Reserve 0x20 (not the 0x18 that would exactly fit f/arg/exit_func), since this value becomes
     * the child thread's initial SP: the RISC-V psABI requires the stack pointer to be
     * 16-byte-aligned at all times (the kernel enforces it on signal delivery and unaligned
     * accesses through SP are UB/fixup-only), and the caller-supplied `stack` here is already
     * 16-aligned (see ALIGN_DOWN_PTR in pal_threading.c's _PalThreadCreate()) -- subtracting a
     * non-multiple-of-16 like 0x18 would misalign it. The extra 8 bytes of padding this leaves
     * are simply unused. */
    addi a1, a1, -0x20
    sd   a0, 0(a1)        /* [a1+0x00] = f */
    sd   a3, 8(a1)        /* [a1+0x08] = arg */
    sd   a7, 16(a1)       /* [a1+0x10] = exit_func */

    /* The raw riscv64 `clone` argument order is flags, stack, parent_tid, tls, child_tid -- the
     * same shape as aarch64 (see finding #3 in the aarch64 port log for why the *LibOS-side*
     * guest-syscall slot order needs its own raw adapter), just with the registers named a0..a4
     * instead of x0..x4. Shuffle through temporaries because the source and destination sets
     * overlap (a2 = old a4 would clobber parent_tid's source otherwise). */
    mv   t0, a2           /* flags */
    mv   t1, a4           /* parent_tid */
    mv   t2, a6           /* child_tid */
    mv   a2, t1           /* parent_tid */
    mv   a3, a5           /* tls */
    mv   a4, t2           /* child_tid */
    mv   a0, t0           /* flags */
    /* a1 already holds the (adjusted) child stack pointer */
    li   a7, __NR_clone
    ecall

    beqz a0, .Lchild
    ret

.Lchild:
    .cfi_undefined x1
    .cfi_undefined x8
    ld   t0, 0(sp)        /* t0 = f */
    ld   a0, 8(sp)        /* a0 = arg (1st/only param for f) */
    ld   t1, 16(sp)       /* t1 = exit_func */
    addi sp, sp, 0x20     /* restore sp to the original `stack` value */
    jalr t0               /* call f(arg); return value lands in a0 */
    jalr t1               /* call exit_func(a0) -- a0 is already positioned as its 1st arg */
    /* Unreachable. */
    ebreak
    .cfi_endproc

.global vfork
.type vfork, %function
.align 4
vfork:
    .cfi_startproc
    /* __NR_vfork does not exist on riscv64's syscall ABI (asm-generic table); emulate via clone()
     * with newsp == 0, which tells the kernel to let the child keep running on the parent's
     * current stack -- exactly vfork's defining semantics. The child resumes right after this
     * `ecall` with a0 == 0 (copy_thread installs the parent's full register state, and unlike
     * x86_64's `syscall` instruction, `ecall` destroys no registers), so it returns from here
     * through the very same `ret` as the parent -- the same shape as the aarch64 emulation. */
    li   a0, (CLONE_VM | CLONE_VFORK | SIGCHLD)
    li   a1, 0            /* newsp = 0: child keeps parent's SP */
    li   a2, 0            /* parent_tid */
    li   a3, 0            /* tls */
    li   a4, 0            /* child_tid */
    li   a7, __NR_clone
    ecall
    ret
    .cfi_endproc

.global syscall_rt_sigreturn
.type syscall_rt_sigreturn, %function
.align 4
syscall_rt_sigreturn:
    .cfi_startproc
    li   a7, __NR_rt_sigreturn
    ecall
    .cfi_endproc

.global pal_arch_sigreturn_frame
.type pal_arch_sigreturn_frame, %function
.align 4
pal_arch_sigreturn_frame:
    .cfi_startproc
    /* noreturn void pal_arch_sigreturn_frame(void* rt_sigframe): rt_sigreturn() from a frame the
     * caller built (see pal_arch_restore_context()); the kernel looks for the frame at `sp`. */
    mv   sp, a0
    li   a7, __NR_rt_sigreturn
    ecall
    .cfi_endproc

.global _PalThreadExit_asm_stub
.type _PalThreadExit_asm_stub, %function
_PalThreadExit_asm_stub:
    .cfi_startproc
    sw   zero, 0(a0)              /* spinlock_unlock(&g_thread_stack_lock) */
    beqz a1, .Lskip_clear_child_tid /* check if clear_child_tid != NULL */
    sw   zero, 0(a1)              /* set *clear_child_tid = 0 */
.Lskip_clear_child_tid:
    li   a0, 0
    li   a7, __NR_exit
    ecall
    j    .Lskip_clear_child_tid   /* Unreachable. */
    .cfi_endproc

gramine_raw_syscalls_code_end:

.section .note.GNU-stack,"",%progbits
