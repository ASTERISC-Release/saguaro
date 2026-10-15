/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/*
 * x86_64 boundary thunks of the VBS-enclave PAL: the x64 twin of pal_vbs_boundary.S (AArch64).
 * GAS (AT&T) syntax, assembled on Linux by clang's integrated assembler
 * (--target=x86_64-pc-windows-msvc). See Documentation/devel/vbs-enclave/PLAN-x64.md, X4.
 *
 * Windows x64 passes arguments in rcx, rdx, r8, r9 (then the stack, above 32 bytes of shadow space)
 * and treats rbx, rbp, rdi, rsi, r12-r15 and xmm6-xmm15 as callee-saved; System V passes them in
 * rdi, rsi, rdx, rcx, r8, r9 and treats rdi, rsi and all xmm registers as caller-saved. So the
 * Windows -> System V thunk saves rdi, rsi and xmm6-xmm15 itself. GS holds the enclave thread's TEB
 * and is never written by packed code. The names of the AArch64 version are kept (read_x18, tpidr):
 * on x64 "x18" means the TEB register (GS base) and "TPIDR_EL0" the FS base, the Linux thread
 * pointer.
 */

    .text

/* uint64_t pal_vbs_read_x18(void): the GS base, i.e. the TEB */
    .globl  pal_vbs_read_x18
    .p2align 4
pal_vbs_read_x18:
    rdgsbase %rax
    ret

/*
 * uint64_t pal_vbs_enter_sysv(void* fn, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3);
 * Calls the System V function fn(a0, a1, a2, a3) from Windows code.
 */
    .globl  pal_vbs_enter_sysv
    .p2align 4
    .seh_proc pal_vbs_enter_sysv
pal_vbs_enter_sysv:
    pushq   %rbp
    .seh_pushreg %rbp
    pushq   %rdi
    .seh_pushreg %rdi
    pushq   %rsi
    .seh_pushreg %rsi
    pushq   %rbx
    .seh_pushreg %rbx
    subq    $168, %rsp
    .seh_stackalloc 168
    movaps  %xmm6, 0(%rsp)
    .seh_savexmm %xmm6, 0
    movaps  %xmm7, 16(%rsp)
    .seh_savexmm %xmm7, 16
    movaps  %xmm8, 32(%rsp)
    .seh_savexmm %xmm8, 32
    movaps  %xmm9, 48(%rsp)
    .seh_savexmm %xmm9, 48
    movaps  %xmm10, 64(%rsp)
    .seh_savexmm %xmm10, 64
    movaps  %xmm11, 80(%rsp)
    .seh_savexmm %xmm11, 80
    movaps  %xmm12, 96(%rsp)
    .seh_savexmm %xmm12, 96
    movaps  %xmm13, 112(%rsp)
    .seh_savexmm %xmm13, 112
    movaps  %xmm14, 128(%rsp)
    .seh_savexmm %xmm14, 128
    movaps  %xmm15, 144(%rsp)
    .seh_savexmm %xmm15, 144
    .seh_endprologue
    movq    %rcx, %rax              /* fn */
    movq    %rdx, %rdi              /* a0 */
    movq    %r8, %rsi               /* a1 */
    movq    %r9, %rdx               /* a2 */
    movq    240(%rsp), %rcx         /* a3: [entry rsp + 40] = rsp + 168 + 32 + 40 */
    callq   *%rax
    movaps  0(%rsp), %xmm6
    movaps  16(%rsp), %xmm7
    movaps  32(%rsp), %xmm8
    movaps  48(%rsp), %xmm9
    movaps  64(%rsp), %xmm10
    movaps  80(%rsp), %xmm11
    movaps  96(%rsp), %xmm12
    movaps  112(%rsp), %xmm13
    movaps  128(%rsp), %xmm14
    movaps  144(%rsp), %xmm15
    addq    $168, %rsp
    popq    %rbx
    popq    %rsi
    popq    %rdi
    popq    %rbp
    retq
    .seh_endproc

/* int64_t pal_vbs_brk0(void): ud2 (caught by SEH and resumed past, 2 bytes), then returns 0 */
    .globl  pal_vbs_brk0
    .p2align 4
pal_vbs_brk0:
    ud2
    xorl    %eax, %eax
    retq

/*
 * Boot context: rbx, rbp, rdi, rsi, r12-r15, the caller's rsp and return address, the FS base,
 * MXCSR and the x87 control word, xmm6-xmm15 (struct pal_vbs_ctx, 256 bytes).
 *
 * int64_t pal_vbs_ctx_save(struct pal_vbs_ctx* c);          returns 0, or v when resumed
 * void    pal_vbs_ctx_resume(struct pal_vbs_ctx* c, int64_t v);   does not return
 */
    .globl  pal_vbs_ctx_save
    .p2align 4
pal_vbs_ctx_save:
    movq    %rbx, 0(%rcx)
    movq    %rbp, 8(%rcx)
    movq    %rdi, 16(%rcx)
    movq    %rsi, 24(%rcx)
    movq    %r12, 32(%rcx)
    movq    %r13, 40(%rcx)
    movq    %r14, 48(%rcx)
    movq    %r15, 56(%rcx)
    leaq    8(%rsp), %rdx
    movq    %rdx, 64(%rcx)          /* rsp after the return */
    movq    (%rsp), %rdx
    movq    %rdx, 72(%rcx)          /* the return address */
    rdfsbase %rdx
    movq    %rdx, 80(%rcx)
    stmxcsr 88(%rcx)
    fnstcw  92(%rcx)
    movdqu  %xmm6, 96(%rcx)
    movdqu  %xmm7, 112(%rcx)
    movdqu  %xmm8, 128(%rcx)
    movdqu  %xmm9, 144(%rcx)
    movdqu  %xmm10, 160(%rcx)
    movdqu  %xmm11, 176(%rcx)
    movdqu  %xmm12, 192(%rcx)
    movdqu  %xmm13, 208(%rcx)
    movdqu  %xmm14, 224(%rcx)
    movdqu  %xmm15, 240(%rcx)
    xorl    %eax, %eax
    retq

    .globl  pal_vbs_ctx_resume
    .p2align 4
pal_vbs_ctx_resume:
    movq    0(%rcx), %rbx
    movq    8(%rcx), %rbp
    movq    16(%rcx), %rdi
    movq    24(%rcx), %rsi
    movq    32(%rcx), %r12
    movq    40(%rcx), %r13
    movq    48(%rcx), %r14
    movq    56(%rcx), %r15
    movq    80(%rcx), %rax
    wrfsbase %rax
    ldmxcsr 88(%rcx)
    fldcw   92(%rcx)
    movdqu  96(%rcx), %xmm6
    movdqu  112(%rcx), %xmm7
    movdqu  128(%rcx), %xmm8
    movdqu  144(%rcx), %xmm9
    movdqu  160(%rcx), %xmm10
    movdqu  176(%rcx), %xmm11
    movdqu  192(%rcx), %xmm12
    movdqu  208(%rcx), %xmm13
    movdqu  224(%rcx), %xmm14
    movdqu  240(%rcx), %xmm15
    movq    64(%rcx), %rsp
    movq    %rdx, %rax
    jmpq    *72(%rcx)

/*
 * uint64_t pal_vbs_x18_spin(uint64_t iterations, uint64_t magic): on x64 the counterpart of the
 * AArch64 x18 measurement is the FS base: puts `magic` there, spins without calling anything, and
 * returns what the FS base then holds (FINDINGS-x64.md: preemption resets it to 0). Restores 0.
 */
    .globl  pal_vbs_x18_spin
    .p2align 4
pal_vbs_x18_spin:
    wrfsbase %rdx
1:  decq    %rcx
    jnz     1b
    rdfsbase %rax
    xorl    %ecx, %ecx
    wrfsbase %rcx
    retq

/*
 * uint64_t pal_vbs_spin_on_stack(uint64_t iterations, void* stack_top);
 * Switches rsp to `stack_top` (outside the thread's Windows stack), spins, switches back.
 */
    .globl  pal_vbs_spin_on_stack
    .p2align 4
pal_vbs_spin_on_stack:
    movq    %rsp, %r8
    andq    $-16, %rdx
    movq    %rdx, %rsp
1:  decq    %rcx
    jnz     1b
    movq    %r8, %rsp
    xorl    %eax, %eax
    retq

/*
 * int64_t pal_vbs_brk_on_stack(void* stack_top);
 * ud2 with rsp outside the Windows stack (caught by SEH and resumed past if the platform can
 * dispatch it there), then switches back.
 */
    .globl  pal_vbs_brk_on_stack
    .p2align 4
pal_vbs_brk_on_stack:
    movq    %rsp, %r8
    subq    $64, %rcx               /* strictly below the top: a frame at StackBase is out of bounds */
    andq    $-16, %rcx
    movq    %rcx, %rsp
    ud2
    movq    %r8, %rsp
    xorl    %eax, %eax
    retq

/*
 * int64_t pal_vbs_brk_probe(void* stack_top);
 * Like pal_vbs_brk_on_stack, but its unwind data (no unwind codes) names the language-specific
 * handler pal_vbs_probe_handler, as pack.py's static .pdata does for packed code.
 */
    .globl  pal_vbs_brk_probe
    .p2align 4
    .seh_proc pal_vbs_brk_probe
    .seh_handler pal_vbs_probe_handler, @except
pal_vbs_brk_probe:
    .seh_endprologue
    movq    %rsp, %r8
    subq    $64, %rcx               /* strictly below the top: a frame at StackBase is out of bounds */
    andq    $-16, %rcx
    movq    %rcx, %rsp
    ud2
    movq    %r8, %rsp
    xorl    %eax, %eax
    retq
    .seh_endproc

/*
 * uint64_t pal_vbs_regs_spin(uint64_t iterations, uint64_t* out_tpidr);
 * Puts known patterns in the FS base, rbx, rbp, rsi, rdi, r8-r15 and xmm0-xmm7, spins without
 * calling anything, and returns a bit mask of what changed: bit i for the general registers in the
 * order rbx rbp rsi rdi r8..r15 (0-11), bit 32+j for xmmj, bit 63 for the FS base. The FS base
 * afterwards goes to *out_tpidr; it is then set to 0, the callee-saved registers are restored.
 */
    .globl  pal_vbs_regs_spin
    .p2align 4
    .seh_proc pal_vbs_regs_spin
pal_vbs_regs_spin:
    pushq   %rbx
    .seh_pushreg %rbx
    pushq   %rbp
    .seh_pushreg %rbp
    pushq   %rsi
    .seh_pushreg %rsi
    pushq   %rdi
    .seh_pushreg %rdi
    pushq   %r12
    .seh_pushreg %r12
    pushq   %r13
    .seh_pushreg %r13
    pushq   %r14
    .seh_pushreg %r14
    pushq   %r15
    .seh_pushreg %r15
    subq    $40, %rsp
    .seh_stackalloc 40
    movdqu  %xmm6, 0(%rsp)
    .seh_savexmm %xmm6, 0
    movdqu  %xmm7, 16(%rsp)
    .seh_savexmm %xmm7, 16
    .seh_endprologue
    movq    %rdx, 32(%rsp)          /* out_tpidr */
    movq    %rcx, %rax              /* iterations */
    movabsq $0x00007ee300000100, %rcx
    wrfsbase %rcx
    movabsq $0x1111000000000000, %rcx
    leaq    1(%rcx), %rbx
    leaq    2(%rcx), %rbp
    leaq    3(%rcx), %rsi
    leaq    4(%rcx), %rdi
    leaq    5(%rcx), %r8
    leaq    6(%rcx), %r9
    leaq    7(%rcx), %r10
    leaq    8(%rcx), %r11
    leaq    9(%rcx), %r12
    leaq    10(%rcx), %r13
    leaq    11(%rcx), %r14
    leaq    12(%rcx), %r15
    movq    %rbx, %xmm0
    movq    %rbp, %xmm1
    movq    %rsi, %xmm2
    movq    %rdi, %xmm3
    movq    %r8, %xmm4
    movq    %r9, %xmm5
    movq    %r10, %xmm6
    movq    %r11, %xmm7
1:  decq    %rax
    jnz     1b
    xorl    %eax, %eax
    movabsq $0x1111000000000000, %rcx
    leaq    1(%rcx), %rdx
    cmpq    %rdx, %rbx
    je      2f
    orq     $1, %rax
2:  leaq    2(%rcx), %rdx
    cmpq    %rdx, %rbp
    je      2f
    orq     $2, %rax
2:  leaq    3(%rcx), %rdx
    cmpq    %rdx, %rsi
    je      2f
    orq     $4, %rax
2:  leaq    4(%rcx), %rdx
    cmpq    %rdx, %rdi
    je      2f
    orq     $8, %rax
2:  leaq    5(%rcx), %rdx
    cmpq    %rdx, %r8
    je      2f
    orq     $0x10, %rax
2:  leaq    6(%rcx), %rdx
    cmpq    %rdx, %r9
    je      2f
    orq     $0x20, %rax
2:  leaq    7(%rcx), %rdx
    cmpq    %rdx, %r10
    je      2f
    orq     $0x40, %rax
2:  leaq    8(%rcx), %rdx
    cmpq    %rdx, %r11
    je      2f
    orq     $0x80, %rax
2:  leaq    9(%rcx), %rdx
    cmpq    %rdx, %r12
    je      2f
    orq     $0x100, %rax
2:  leaq    10(%rcx), %rdx
    cmpq    %rdx, %r13
    je      2f
    orq     $0x200, %rax
2:  leaq    11(%rcx), %rdx
    cmpq    %rdx, %r14
    je      2f
    orq     $0x400, %rax
2:  leaq    12(%rcx), %rdx
    cmpq    %rdx, %r15
    je      2f
    orq     $0x800, %rax
2:  movq    %xmm0, %rdx
    cmpq    %rdx, %rbx
    je      2f
    btsq    $32, %rax
2:  movq    %xmm1, %rdx
    cmpq    %rdx, %rbp
    je      2f
    btsq    $33, %rax
2:  movq    %xmm2, %rdx
    cmpq    %rdx, %rsi
    je      2f
    btsq    $34, %rax
2:  movq    %xmm3, %rdx
    cmpq    %rdx, %rdi
    je      2f
    btsq    $35, %rax
2:  movq    %xmm4, %rdx
    cmpq    %rdx, %r8
    je      2f
    btsq    $36, %rax
2:  movq    %xmm5, %rdx
    cmpq    %rdx, %r9
    je      2f
    btsq    $37, %rax
2:  movq    %xmm6, %rdx
    cmpq    %rdx, %r10
    je      2f
    btsq    $38, %rax
2:  movq    %xmm7, %rdx
    cmpq    %rdx, %r11
    je      2f
    btsq    $39, %rax
2:  rdfsbase %rdx
    movq    32(%rsp), %rcx
    movq    %rdx, (%rcx)
    movabsq $0x00007ee300000100, %rcx
    cmpq    %rcx, %rdx
    je      2f
    btsq    $63, %rax
2:  xorl    %ecx, %ecx
    wrfsbase %rcx
    movdqu  0(%rsp), %xmm6
    movdqu  16(%rsp), %xmm7
    addq    $40, %rsp
    popq    %r15
    popq    %r14
    popq    %r13
    popq    %r12
    popq    %rdi
    popq    %rsi
    popq    %rbp
    popq    %rbx
    retq
    .seh_endproc

/* uint64_t pal_vbs_get_tpidr(void), void pal_vbs_set_tpidr(uint64_t v): the FS base */
    .globl  pal_vbs_get_tpidr
    .p2align 4
pal_vbs_get_tpidr:
    rdfsbase %rax
    retq

    .globl  pal_vbs_set_tpidr
    .p2align 4
pal_vbs_set_tpidr:
    wrfsbase %rcx
    retq

/*
 * uint64_t pal_vbs_tpidr_spin(uint64_t iterations, uint64_t magic, uint64_t* out_first);
 * Sets the FS base to `magic` and compares it on every iteration of a tight loop; counts the
 * changes (setting it again after each) and stores the first different value. Restores 0.
 */
    .globl  pal_vbs_tpidr_spin
    .p2align 4
pal_vbs_tpidr_spin:
    wrfsbase %rdx
    xorl    %eax, %eax
    movq    $0, (%r8)
1:  rdfsbase %r9
    cmpq    %rdx, %r9
    je      3f
    testq   %rax, %rax
    jnz     2f
    movq    %r9, (%r8)
2:  incq    %rax
    wrfsbase %rdx
3:  decq    %rcx
    jnz     1b
    xorl    %ecx, %ecx
    wrfsbase %rcx
    retq
