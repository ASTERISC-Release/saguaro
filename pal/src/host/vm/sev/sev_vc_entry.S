/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/*
 * #VC entry of SEV-ES/SNP guests (sev_vc.c). It builds the `struct isr_regs` frame of
 * kernel_events.S, but saves no XSAVE state and does not touch %gs: the C handler is built with
 * -mgeneral-regs-only and without the stack protector. Before the kernel's IDT is loaded,
 * sev_vc_early_idt (vector 29 only, no IST) catches the #VCs of early boot on the boot stack.
 */

.macro globl sym
.globl \sym
.hidden \sym
.endm

    globl   isr_vc
    globl   sev_vc_early_idtr
    globl   sev_vc_early_idt_init

    .text
isr_vc:
    // the CPU pushed the error code (the SVM exit code)
    push    $29
    push    %rcx
    push    %rax
    push    %rdx
    push    %rbx
    push    %rbp
    push    %rsi
    push    %rdi
    push    %r15
    push    %r14
    push    %r13
    push    %r12
    push    %r11
    push    %r10
    push    %r9
    push    %r8
    push    $0                  // isr_regs.gs_swapped
    push    $0                  // isr_regs.fpregs (none)

    mov     %rsp, %rdi
    mov     %rsp, %rbx
    and     $~0xF, %rsp
    cld
    call    sev_vc_entry_c
    mov     %rbx, %rsp

    add     $16, %rsp           // fpregs, gs_swapped
    pop     %r8
    pop     %r9
    pop     %r10
    pop     %r11
    pop     %r12
    pop     %r13
    pop     %r14
    pop     %r15
    pop     %rdi
    pop     %rsi
    pop     %rbp
    pop     %rbx
    pop     %rdx
    pop     %rax
    pop     %rcx
    add     $16, %rsp           // int_number, error_code
    iretq

    // void sev_vc_early_idt_init(void): fill gate 29 of the early IDT (64-bit interrupt gate,
    // kernel CS of the current GDT, no IST) and load it
sev_vc_early_idt_init:
    lea     isr_vc(%rip), %rax
    lea     sev_vc_early_idt + 29 * 16(%rip), %rdi
    mov     %ax, (%rdi)                 // offset 15:0
    mov     %cs, %dx
    mov     %dx, 2(%rdi)                // selector
    movb    $0, 4(%rdi)                 // IST
    movb    $0x8e, 5(%rdi)              // present, DPL 0, 64-bit interrupt gate
    shr     $16, %rax
    mov     %ax, 6(%rdi)                // offset 31:16
    shr     $16, %rax
    mov     %eax, 8(%rdi)               // offset 63:32
    movl    $0, 12(%rdi)
    lea     sev_vc_early_idt(%rip), %rax
    mov     %rax, sev_vc_early_idtr + 2(%rip)   // base filled at run time (before relocation)
    lidt    sev_vc_early_idtr(%rip)
    ret

    .data
    .align  16
sev_vc_early_idt:
    .fill   30 * 16, 1, 0
sev_vc_early_idtr:
    .word   30 * 16 - 1
    .quad   0
