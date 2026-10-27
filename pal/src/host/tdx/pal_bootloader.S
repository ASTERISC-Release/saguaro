/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2023 Intel Corporation */

.macro globl sym
.globl \sym
.hidden \sym
.endm

    globl   pal_start

    .code64
    .align  0x1000

//
// TD-Shim loads this TDX PAL binary as the payload with arguments:
//   - RDI: holds the payload HOB address
//   - RSI: holds the address where the payload is loaded
//
// This entry code must be careful to not modify RDI and RSI, to forward them as-is to the C-level
// pal_start_c() function.
//
// This code setups GDT, IDT, CR4, XCR0, all data segment regs and CS segment reg.
//
pal_start:
    // the #VE gate (vector 20) of our IDT points at _early_ve_handler; its address is known only at
    // run time (the PAL is position-independent), so the gate is filled here
    lea     _early_ve_handler(%rip), %rax
    lea     _idt_start + 20 * 16(%rip), %rcx
    mov     %ax, (%rcx)                         // VA[15..0]
    shr     $16, %rax
    mov     %ax, 6(%rcx)                        // VA[31..16]
    shr     $16, %rax
    mov     %eax, 8(%rcx)                       // VA[63..32]

    // set up our own GDT and IDT; this is important because e.g. TD-Shim installs its IDT that has
    // a #VE exception handler (that simply forwards requests to host VMM without sanitization)
    // TODO: this is probably not needed because our vm-common code resets these anyway
    lea     _gdtr(%rip), %rax
    lgdt    (%rax)
    lea     _idtr(%rip), %rax
    lidt    (%rax)

    // set data segment registers to our GDT "64-bit kernel data" segment descriptor
    mov     $(_gdt_entry_kernel_cs - _gdt_start + 8), %rax
    mov     %rax, %ds
    mov     %rax, %es
    mov     %rax, %fs
    mov     %rax, %gs
    mov     %rax, %ss

    // update CR4 to include OSXSAVE, OSXMMEXCPT, OSFXSR
    mov     %cr4, %rax
    or      $(1 << 18) + (1 << 10) + (1 << 9), %rax
    mov     %rax, %cr4

    // set XCR0 to enable SSE and X87 (RCX: which XCR reg to update, RDX: upper bits of features);
    // PAL will enable more features if available on VM
    mov     $3, %rax
    mov     $0, %rcx
    mov     $0, %rdx
    xsetbv

    // use our own initial stack and clear frame pointer (RBP) for sanity; note that we need to
    // conform to System V AMD64 ABI, thus we set an 8-byte-odd RSP on func entry
    xor     %rbp, %rbp
    lea     _sys_initial_stack(%rip), %rsp
    sub     $8, %rsp

    // jump to pal_start_c() entrypoint, together with setting the CS segment register
    lea     pal_start_c(%rip), %rax
    pushq   $(_gdt_entry_kernel_cs - _gdt_start)
    push    %rax
    lretq

//
// A #VE before interrupts_init() installs the PAL's exception handlers (an instruction the TDX module
// does not virtualize, an access beyond the TD's GPAW, ...): without this handler, the gate would
// lead to address 0 and the TD would end in a silent triple fault. Print the exit reason, the RIP
// and the GPA on the serial port (TDG.VP.VMCALL<Instruction.IO>, as the PAL's early log does), then
// end the TD with a triple fault (an empty IDT), which QEMU reports as a reset of the guest.
//
_early_ve_handler:
    mov     $3, %eax                            // TDG.VP.VEINFO.GET
    .byte   0x66, 0x0f, 0x01, 0xcc              // TDCALL: RCX exit reason, R9 GPA
    push    %r9
    push    %rcx
    lea     _early_ve_msg_reason(%rip), %rbx
    call    _early_puts
    mov     (%rsp), %ebp                        // exit reason (32 bits)
    call    _early_puthex
    lea     _early_ve_msg_rip(%rip), %rbx
    call    _early_puts
    mov     16(%rsp), %rbp                      // RIP of the exception frame (#VE pushes no code)
    call    _early_puthex
    lea     _early_ve_msg_gpa(%rip), %rbx
    call    _early_puts
    mov     8(%rsp), %rbp                       // GPA
    call    _early_puthex
    lea     _early_ve_msg_end(%rip), %rbx
    call    _early_puts
    lidt    _early_null_idtr(%rip)
    ud2                                         // #UD, #GP, #DF: a triple fault

// write the string at RBX (NUL-terminated) to the serial port; clobbers RAX, RCX, RDX, R10-R15
_early_puts:
    movzbl  (%rbx), %edx
    test    %edx, %edx
    jz      1f
    call    _early_putc
    inc     %rbx
    jmp     _early_puts
1:
    ret

// write RBP as 16 hex digits to the serial port; clobbers RAX, RBX, RCX, RDX, R10-R15
_early_puthex:
    mov     $16, %ebx
1:
    rol     $4, %rbp
    mov     %ebp, %edx
    and     $0xf, %edx
    add     $'0', %edx
    cmp     $'9', %edx
    jbe     2f
    add     $('a' - '0' - 10), %edx
2:
    call    _early_putc
    dec     %ebx
    jnz     1b
    ret

// write the byte in DL to the serial port 0x3F8 through TDG.VP.VMCALL<Instruction.IO>; the TDX
// module passes only R10-R15 to the host VMM (RCX mask) and keeps the other registers
_early_putc:
    xor     %eax, %eax                          // TDG.VP.VMCALL
    mov     $0xfc00, %ecx                       // pass R10-R15
    xor     %r10d, %r10d
    mov     $0x1e, %r11d                        // Instruction.IO
    mov     $1, %r12d                           // 1 byte
    mov     $1, %r13d                           // write
    mov     $0x3f8, %r14d                       // COM1
    movzbl  %dl, %r15d
    .byte   0x66, 0x0f, 0x01, 0xcc              // TDCALL
    ret


    .data
    .align  0x1000

// IDT
_idt_start:
    .rept   256
    .word   0                                   // VA[15..0] of ISR
    .word   _gdt_entry_kernel_cs - _gdt_start   // code segment selector
    .byte   0                                   // Interrupt Task Table offset
    .byte   0b10001110                          // attributes (P=1, DPL=0, RSV=0, type=1110)
    .word   0                                   // VA[31..16] of ISR
    .long   0                                   // VA[64..32] of ISR
    .long   0                                   // Reserved
    .endr
_idtr:
    .word . - _idt_start - 1
    .quad _idt_start
_early_null_idtr:
    .word 0
    .quad 0

_early_ve_msg_reason:
    .asciz  "\nPAL: #VE before the exception handlers are set up: exit reason 0x"
_early_ve_msg_rip:
    .asciz  ", RIP 0x"
_early_ve_msg_gpa:
    .asciz  ", GPA 0x"
_early_ve_msg_end:
    .asciz  "; stopping the TD\n"

// GDT
    .align  0x10
_gdt_start:
    .quad   0
_gdt_entry_kernel_cs:
    .long   0xffff, 0xaf9b00                    // 64-bit kernel code
    .long   0xffff, 0xaf9300                    // 64-bit kernel data
_gdt_entry_user_cs:
    .long   0xffff, 0xcffb00                    // 32-bit user code
    .long   0xffff, 0xcff300                    // 32-bit user data
    .long   0xffff, 0xaffb00                    // 64-bit user code
_tss_64bitmode_desc:
    .rept   1                   // currently only one CPU core supported
    .word   0                   // segment limit in bytes, bits 0-15 (will be filled in C)
    .word   0                   // segment base address, bits 0-15   (will be filled in C)
    .byte   0                   // segment base address, bits 16-23  (will be filled in C)
    .byte   0b10001001          // flags, bits 24-31 (P=1, DPL=00, RVD=0, Type=1001 (inactive task))
    .byte   0                   // segment limit, bits 16-19         (set to zero)
                                // and flags, bits 20-23 (G=0, RVD=00, AVL=0)
    .byte   0                   // segment base address, bits 24-31  (will be filled in C)
    .long   0                   // segment base address, bits 32-63  (will be filled in C)
    .long   0                   // reserved space
    .endr
_gdtr:
    .word   . - _gdt_start - 1
    .quad   _gdt_start

    .bss
    .align  0x10
    .space  0x4000
_sys_initial_stack:
