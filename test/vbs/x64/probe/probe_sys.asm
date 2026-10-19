; Copyright (C) 2026 The ASTERISC Research Lab
; Gramine x64 phase-0 spike: the Linux system call instructions, linked into the enclave only (a
; host executable that contains a raw syscall instruction is more likely to be refused by Smart App
; Control's reputation check).

PUBLIC g_after_syscall_rcx
PUBLIC g_after_syscall_r11

_DATA SEGMENT
g_after_syscall_rcx DQ 0
g_after_syscall_r11 DQ 0
_DATA ENDS

_TEXT SEGMENT

; long long probe_syscall(long long nr, long long a0, long long a1, long long a2):
; a Linux x86_64 system call: rax = nr, rdi = a0, rsi = a1, rdx = a2; syscall.
probe_syscall PROC FRAME
    push    rdi
    .pushreg rdi
    push    rsi
    .pushreg rsi
    .endprolog
    mov     rax, rcx
    mov     rdi, rdx
    mov     rsi, r8
    mov     rdx, r9
    mov     rcx, 0C0C0C0C0C0C0C0C0h
    mov     r11, 0B1B1B1B1B1B1B1B1h
    syscall
    mov     g_after_syscall_rcx, rcx
    mov     g_after_syscall_r11, r11
    pop     rsi
    pop     rdi
    ret
probe_syscall ENDP

; long long probe_int80(long long nr): the 32-bit Linux system call gate, eax = nr
probe_int80 PROC
    mov     rax, rcx
    int     80h
    ret
probe_int80 ENDP

_TEXT ENDS

END
