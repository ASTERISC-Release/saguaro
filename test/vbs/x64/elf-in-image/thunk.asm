; Copyright (C) 2026 The ASTERISC Research Lab
; Saguaro x64 phase-0 spike (step h): the Windows x64 to System V thunk.

_TEXT SEGMENT

; long long call_sysv(void *fn, long long a0): calls the System V function fn(a0) from Windows x64
; code. System V treats rsi, rdi and xmm6-xmm15 as caller-saved, Windows as callee-saved: save them.
; rsp is 16-byte aligned at the call, as System V requires.
call_sysv PROC FRAME
    push    rdi
    .pushreg rdi
    push    rsi
    .pushreg rsi
    sub     rsp, 0A8h
    .allocstack 0A8h
    movdqa  [rsp+00h], xmm6
    .savexmm128 xmm6, 00h
    movdqa  [rsp+10h], xmm7
    .savexmm128 xmm7, 10h
    movdqa  [rsp+20h], xmm8
    .savexmm128 xmm8, 20h
    movdqa  [rsp+30h], xmm9
    .savexmm128 xmm9, 30h
    movdqa  [rsp+40h], xmm10
    .savexmm128 xmm10, 40h
    movdqa  [rsp+50h], xmm11
    .savexmm128 xmm11, 50h
    movdqa  [rsp+60h], xmm12
    .savexmm128 xmm12, 60h
    movdqa  [rsp+70h], xmm13
    .savexmm128 xmm13, 70h
    movdqa  [rsp+80h], xmm14
    .savexmm128 xmm14, 80h
    movdqa  [rsp+90h], xmm15
    .savexmm128 xmm15, 90h
    .endprolog
    mov     rax, rcx
    mov     rdi, rdx
    call    rax
    movdqa  xmm6, [rsp+00h]
    movdqa  xmm7, [rsp+10h]
    movdqa  xmm8, [rsp+20h]
    movdqa  xmm9, [rsp+30h]
    movdqa  xmm10, [rsp+40h]
    movdqa  xmm11, [rsp+50h]
    movdqa  xmm12, [rsp+60h]
    movdqa  xmm13, [rsp+70h]
    movdqa  xmm14, [rsp+80h]
    movdqa  xmm15, [rsp+90h]
    add     rsp, 0A8h
    pop     rsi
    pop     rdi
    ret
call_sysv ENDP

; unsigned long long read_callee_saved_sum(void)
read_callee_saved_sum PROC
    mov     rax, rbx
    add     rax, rbp
    add     rax, rsi
    add     rax, rdi
    add     rax, r12
    add     rax, r13
    add     rax, r14
    add     rax, r15
    ret
read_callee_saved_sum ENDP

_TEXT ENDS
END
