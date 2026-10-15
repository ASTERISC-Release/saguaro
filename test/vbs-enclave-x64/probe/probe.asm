; Copyright (C) 2026 The ASTERISC Research Lab
; Gramine x64 phase-0 spike: x86_64 helpers for the probe enclave (ml64 syntax).
; Windows x64 calling convention: arguments in rcx, rdx, r8, r9; rbx, rbp, rdi, rsi, r12-r15 and
; xmm6-xmm15 are callee-saved. Leaf functions that do not move rsp need no unwind data.

EXTERN __imp_CallEnclave:QWORD
EXTERN probe_lang_handler:PROC

_TEXT SEGMENT

; long long probe_int3(void): int3 (1 byte), then rax = 7
probe_int3 PROC
    int     3
    mov     eax, 7
    ret
probe_int3 ENDP

; long long probe_ud2(void): ud2 (2 bytes), then rax = 9
probe_ud2 PROC
    ud2
    mov     eax, 9
    ret
probe_ud2 ENDP

; long long probe_div0(void): div by zero (div ecx, 2 bytes), then rax = 10
probe_div0 PROC
    xor     edx, edx
    mov     eax, 1
    xor     ecx, ecx
    div     ecx
    mov     eax, 10
    ret
probe_div0 ENDP

; long long probe_hlt(void): a privileged instruction (1 byte), then rax = 11
probe_hlt PROC
    hlt
    mov     eax, 11
    ret
probe_hlt ENDP

; void probe_cpuid(unsigned leaf, unsigned subleaf, unsigned out[4])
probe_cpuid PROC FRAME
    push    rbx
    .pushreg rbx
    .endprolog
    mov     eax, ecx
    mov     ecx, edx
    cpuid
    mov     [r8], eax
    mov     [r8+4], ebx
    mov     [r8+8], ecx
    mov     [r8+12], edx
    pop     rbx
    ret
probe_cpuid ENDP

; unsigned long long probe_xgetbv(unsigned idx)
probe_xgetbv PROC
    xgetbv
    shl     rdx, 32
    or      rax, rdx
    ret
probe_xgetbv ENDP

probe_rdfsbase PROC
    rdfsbase rax
    ret
probe_rdfsbase ENDP

probe_wrfsbase PROC
    wrfsbase rcx
    ret
probe_wrfsbase ENDP

probe_rdgsbase PROC
    rdgsbase rax
    ret
probe_rdgsbase ENDP

; unsigned long long probe_read_rsp(void): rsp of the caller (after return)
probe_read_rsp PROC
    lea     rax, [rsp+8]
    ret
probe_read_rsp ENDP

; long long probe_jump(void *fn, long long a0, long long a1): tail-calls fn(a0, a1) without a CFG check
probe_jump PROC
    mov     rax, rcx
    mov     rcx, rdx
    mov     rdx, r8
    jmp     rax
probe_jump ENDP

; unsigned long long probe_rdtscp(unsigned *aux)
probe_rdtscp PROC
    mov     r8, rcx
    rdtscp
    mov     [r8], ecx
    shl     rdx, 32
    or      rax, rdx
    ret
probe_rdtscp ENDP

; unsigned long long probe_rdrand(int *ok), probe_rdseed(int *ok)
probe_rdrand PROC
    rdrand  rax
    setc    dl
    movzx   edx, dl
    mov     [rcx], edx
    ret
probe_rdrand ENDP

probe_rdseed PROC
    rdseed  rax
    setc    dl
    movzx   edx, dl
    mov     [rcx], edx
    ret
probe_rdseed ENDP

; long long probe_redzone_ud2(void): fills the 128 bytes below rsp with a pattern, executes ud2 (the
; probe's handler resumes after it), then returns a bit mask of the 16 qwords that changed (bit k:
; the qword at rsp-136+8k, so bit 16 is rsp-8 and bit 1 is rsp-128).
probe_redzone_ud2 PROC
    mov     rax, 5A5A5A5A00000000h
    mov     ecx, 1
@@: lea     rdx, [rax+rcx]
    mov     [rsp+rcx*8-136], rdx
    inc     ecx
    cmp     ecx, 17
    jne     @B
    ud2
    xor     r8d, r8d
    mov     ecx, 1
@@: lea     rdx, [rax+rcx]
    cmp     [rsp+rcx*8-136], rdx
    je      @F
    bts     r8, rcx
@@: inc     ecx
    cmp     ecx, 17
    jne     @B
    mov     rax, r8
    ret
probe_redzone_ud2 ENDP

; long long probe_redzone_spin(unsigned long long iters): the same pattern, then a loop of iters
; iterations that calls nothing; returns the bit mask of changed qwords.
probe_redzone_spin PROC
    mov     r9, rcx
    mov     rax, 5A5A5A5A00000000h
    mov     ecx, 1
@@: lea     rdx, [rax+rcx]
    mov     [rsp+rcx*8-136], rdx
    inc     ecx
    cmp     ecx, 17
    jne     @B
@@: dec     r9
    jnz     @B
    xor     r8d, r8d
    mov     ecx, 1
@@: lea     rdx, [rax+rcx]
    cmp     [rsp+rcx*8-136], rdx
    je      @F
    bts     r8, rcx
@@: inc     ecx
    cmp     ecx, 17
    jne     @B
    mov     rax, r8
    ret
probe_redzone_spin ENDP

; long long probe_fs_spin(unsigned long long value, unsigned long long iters, unsigned long long out[3]):
; wrfsbase value, then compares rdfsbase with it on every iteration; counts changes (rewriting the
; value after each change). out[0] = first different value, out[1] = first different NON-ZERO value,
; out[2] = number of changes to a non-zero value. Restores FS base 0 at the end.
probe_fs_spin PROC
    wrfsbase rcx
    xor     eax, eax
    mov     qword ptr [r8], 0
    mov     qword ptr [r8+8], 0
    mov     qword ptr [r8+16], 0
@@: rdfsbase r9
    cmp     r9, rcx
    je      probe_fs_spin_next
    test    rax, rax
    jnz     probe_fs_spin_counted
    mov     [r8], r9
probe_fs_spin_counted:
    inc     rax
    test    r9, r9
    jz      probe_fs_spin_zero
    cmp     qword ptr [r8+16], 0
    jne     probe_fs_spin_nz_counted
    mov     [r8+8], r9
probe_fs_spin_nz_counted:
    inc     qword ptr [r8+16]
probe_fs_spin_zero:
    wrfsbase rcx
probe_fs_spin_next:
    dec     rdx
    jnz     @B
    xor     ecx, ecx
    wrfsbase rcx
    ret
probe_fs_spin ENDP

; long long probe_regs_spin(unsigned long long iters): puts patterns in rbx, rsi, rdi, rbp, r8-r15,
; xmm6-xmm15 and ymm0-ymm5 (upper halves too), spins iters iterations, returns a bit mask of the
; registers that changed (bit 0 rbx, 1 rsi, 2 rdi, 3 rbp, 4-11 r8-r15, 12-21 xmm6-15, 22-27 ymm0-5 upper).
probe_regs_spin PROC FRAME
    push    rbx
    .pushreg rbx
    push    rsi
    .pushreg rsi
    push    rdi
    .pushreg rdi
    push    rbp
    .pushreg rbp
    push    r12
    .pushreg r12
    push    r13
    .pushreg r13
    push    r14
    .pushreg r14
    push    r15
    .pushreg r15
    sub     rsp, 0B8h
    .allocstack 0B8h
    movdqu  [rsp+00h], xmm6
    .savexmm128 xmm6, 00h
    movdqu  [rsp+10h], xmm7
    .savexmm128 xmm7, 10h
    movdqu  [rsp+20h], xmm8
    .savexmm128 xmm8, 20h
    movdqu  [rsp+30h], xmm9
    .savexmm128 xmm9, 30h
    movdqu  [rsp+40h], xmm10
    .savexmm128 xmm10, 40h
    movdqu  [rsp+50h], xmm11
    .savexmm128 xmm11, 50h
    movdqu  [rsp+60h], xmm12
    .savexmm128 xmm12, 60h
    movdqu  [rsp+70h], xmm13
    .savexmm128 xmm13, 70h
    movdqu  [rsp+80h], xmm14
    .savexmm128 xmm14, 80h
    movdqu  [rsp+90h], xmm15
    .savexmm128 xmm15, 90h
    .endprolog
    mov     [rsp+0A0h], rcx
    mov     rax, 1111000000000000h
    lea     rbx, [rax+1]
    lea     rsi, [rax+2]
    lea     rdi, [rax+3]
    lea     rbp, [rax+4]
    lea     r8, [rax+5]
    lea     r9, [rax+6]
    lea     r10, [rax+7]
    lea     r11, [rax+8]
    lea     r12, [rax+9]
    lea     r13, [rax+10]
    lea     r14, [rax+11]
    lea     r15, [rax+12]
    movq    xmm6, rbx
    movq    xmm7, rsi
    movq    xmm8, rdi
    movq    xmm9, rbp
    movq    xmm10, r8
    movq    xmm11, r9
    movq    xmm12, r10
    movq    xmm13, r11
    movq    xmm14, r12
    movq    xmm15, r13
    vpcmpeqd ymm0, ymm0, ymm0
    vpcmpeqd ymm1, ymm1, ymm1
    vpcmpeqd ymm2, ymm2, ymm2
    vpcmpeqd ymm3, ymm3, ymm3
    vpcmpeqd ymm4, ymm4, ymm4
    vpcmpeqd ymm5, ymm5, ymm5
    mov     rcx, [rsp+0A0h]
@@: dec     rcx
    jnz     @B
    xor     ecx, ecx                 ; rcx = result mask
    mov     rax, 1111000000000000h
    lea     rdx, [rax+1]
    cmp     rbx, rdx
    je      @F
    or      ecx, 1
@@: lea     rdx, [rax+2]
    cmp     rsi, rdx
    je      @F
    or      ecx, 2
@@: lea     rdx, [rax+3]
    cmp     rdi, rdx
    je      @F
    or      ecx, 4
@@: lea     rdx, [rax+4]
    cmp     rbp, rdx
    je      @F
    or      ecx, 8
@@: lea     rdx, [rax+5]
    cmp     r8, rdx
    je      @F
    or      ecx, 10h
@@: lea     rdx, [rax+6]
    cmp     r9, rdx
    je      @F
    or      ecx, 20h
@@: lea     rdx, [rax+7]
    cmp     r10, rdx
    je      @F
    or      ecx, 40h
@@: lea     rdx, [rax+8]
    cmp     r11, rdx
    je      @F
    or      ecx, 80h
@@: lea     rdx, [rax+9]
    cmp     r12, rdx
    je      @F
    or      ecx, 100h
@@: lea     rdx, [rax+10]
    cmp     r13, rdx
    je      @F
    or      ecx, 200h
@@: lea     rdx, [rax+11]
    cmp     r14, rdx
    je      @F
    or      ecx, 400h
@@: lea     rdx, [rax+12]
    cmp     r15, rdx
    je      @F
    or      ecx, 800h
@@: movq    rdx, xmm6
    cmp     rdx, rbx
    je      @F
    or      ecx, 1000h
@@: movq    rdx, xmm7
    cmp     rdx, rsi
    je      @F
    or      ecx, 2000h
@@: movq    rdx, xmm8
    cmp     rdx, rdi
    je      @F
    or      ecx, 4000h
@@: movq    rdx, xmm9
    cmp     rdx, rbp
    je      @F
    or      ecx, 8000h
@@: movq    rdx, xmm10
    cmp     rdx, r8
    je      @F
    or      ecx, 10000h
@@: movq    rdx, xmm11
    cmp     rdx, r9
    je      @F
    or      ecx, 20000h
@@: movq    rdx, xmm12
    cmp     rdx, r10
    je      @F
    or      ecx, 40000h
@@: movq    rdx, xmm13
    cmp     rdx, r11
    je      @F
    or      ecx, 80000h
@@: movq    rdx, xmm14
    cmp     rdx, r12
    je      @F
    or      ecx, 100000h
@@: movq    rdx, xmm15
    cmp     rdx, r13
    je      @F
    or      ecx, 200000h
@@: vextracti128 xmm6, ymm0, 1
    movq    rdx, xmm6
    cmp     rdx, -1
    je      @F
    or      ecx, 400000h
@@: vextracti128 xmm6, ymm1, 1
    movq    rdx, xmm6
    cmp     rdx, -1
    je      @F
    or      ecx, 800000h
@@: vextracti128 xmm6, ymm2, 1
    movq    rdx, xmm6
    cmp     rdx, -1
    je      @F
    or      ecx, 1000000h
@@: vextracti128 xmm6, ymm3, 1
    movq    rdx, xmm6
    cmp     rdx, -1
    je      @F
    or      ecx, 2000000h
@@: vextracti128 xmm6, ymm4, 1
    movq    rdx, xmm6
    cmp     rdx, -1
    je      @F
    or      ecx, 4000000h
@@: vextracti128 xmm6, ymm5, 1
    movq    rdx, xmm6
    cmp     rdx, -1
    je      @F
    or      ecx, 8000000h
@@: vzeroupper
    mov     eax, ecx
    movdqu  xmm6, [rsp+00h]
    movdqu  xmm7, [rsp+10h]
    movdqu  xmm8, [rsp+20h]
    movdqu  xmm9, [rsp+30h]
    movdqu  xmm10, [rsp+40h]
    movdqu  xmm11, [rsp+50h]
    movdqu  xmm12, [rsp+60h]
    movdqu  xmm13, [rsp+70h]
    movdqu  xmm14, [rsp+80h]
    movdqu  xmm15, [rsp+90h]
    add     rsp, 0B8h
    pop     r15
    pop     r14
    pop     r13
    pop     r12
    pop     rbp
    pop     rdi
    pop     rsi
    pop     rbx
    ret
probe_regs_spin ENDP

; long long probe_callout_regs(void *host_fn): puts patterns in the callee-saved registers rbx, rsi,
; rdi, rbp, r12-r15 and xmm6-xmm15, calls CallEnclave(host_fn, 0x77, TRUE, &ret) (an exit to the
; host and back), and returns a bit mask of the callee-saved registers that changed (bit 0 rbx, 1 rsi,
; 2 rdi, 3 rbp, 4-7 r12-r15, 8-17 xmm6-15), with bit 31 set if CallEnclave failed.
probe_callout_regs PROC FRAME
    push    rbx
    .pushreg rbx
    push    rsi
    .pushreg rsi
    push    rdi
    .pushreg rdi
    push    rbp
    .pushreg rbp
    push    r12
    .pushreg r12
    push    r13
    .pushreg r13
    push    r14
    .pushreg r14
    push    r15
    .pushreg r15
    sub     rsp, 0D8h
    .allocstack 0D8h
    movdqu  [rsp+30h], xmm6
    .savexmm128 xmm6, 30h
    movdqu  [rsp+40h], xmm7
    .savexmm128 xmm7, 40h
    movdqu  [rsp+50h], xmm8
    .savexmm128 xmm8, 50h
    movdqu  [rsp+60h], xmm9
    .savexmm128 xmm9, 60h
    movdqu  [rsp+70h], xmm10
    .savexmm128 xmm10, 70h
    movdqu  [rsp+80h], xmm11
    .savexmm128 xmm11, 80h
    movdqu  [rsp+90h], xmm12
    .savexmm128 xmm12, 90h
    movdqu  [rsp+0A0h], xmm13
    .savexmm128 xmm13, 0A0h
    movdqu  [rsp+0B0h], xmm14
    .savexmm128 xmm14, 0B0h
    movdqu  [rsp+0C0h], xmm15
    .savexmm128 xmm15, 0C0h
    .endprolog
    mov     rax, 2222000000000000h
    lea     rbx, [rax+1]
    lea     rsi, [rax+2]
    lea     rdi, [rax+3]
    lea     rbp, [rax+4]
    lea     r12, [rax+5]
    lea     r13, [rax+6]
    lea     r14, [rax+7]
    lea     r15, [rax+8]
    movq    xmm6, rbx
    movq    xmm7, rsi
    movq    xmm8, rdi
    movq    xmm9, rbp
    movq    xmm10, r12
    movq    xmm11, r13
    movq    xmm12, r14
    movq    xmm13, r15
    movq    xmm14, rbx
    movq    xmm15, rsi
    ; CallEnclave(host_fn, 0x77, TRUE, &ret): rcx = host_fn already
    mov     edx, 77h
    mov     r8d, 1
    lea     r9, [rsp+28h]
    call    qword ptr [__imp_CallEnclave]
    xor     ecx, ecx
    test    eax, eax
    jnz     @F
    mov     ecx, 80000000h
@@: mov     rax, 2222000000000000h
    lea     rdx, [rax+1]
    cmp     rbx, rdx
    je      @F
    or      ecx, 1
@@: lea     rdx, [rax+2]
    cmp     rsi, rdx
    je      @F
    or      ecx, 2
@@: lea     rdx, [rax+3]
    cmp     rdi, rdx
    je      @F
    or      ecx, 4
@@: lea     rdx, [rax+4]
    cmp     rbp, rdx
    je      @F
    or      ecx, 8
@@: lea     rdx, [rax+5]
    cmp     r12, rdx
    je      @F
    or      ecx, 10h
@@: lea     rdx, [rax+6]
    cmp     r13, rdx
    je      @F
    or      ecx, 20h
@@: lea     rdx, [rax+7]
    cmp     r14, rdx
    je      @F
    or      ecx, 40h
@@: lea     rdx, [rax+8]
    cmp     r15, rdx
    je      @F
    or      ecx, 80h
@@: movq    rdx, xmm6
    cmp     rdx, rbx
    je      @F
    or      ecx, 100h
@@: movq    rdx, xmm7
    cmp     rdx, rsi
    je      @F
    or      ecx, 200h
@@: movq    rdx, xmm8
    cmp     rdx, rdi
    je      @F
    or      ecx, 400h
@@: movq    rdx, xmm9
    cmp     rdx, rbp
    je      @F
    or      ecx, 800h
@@: movq    rdx, xmm10
    cmp     rdx, r12
    je      @F
    or      ecx, 1000h
@@: movq    rdx, xmm11
    cmp     rdx, r13
    je      @F
    or      ecx, 2000h
@@: movq    rdx, xmm12
    cmp     rdx, r14
    je      @F
    or      ecx, 4000h
@@: movq    rdx, xmm13
    cmp     rdx, r15
    je      @F
    or      ecx, 8000h
@@: movq    rdx, xmm14
    cmp     rdx, rbx
    je      @F
    or      ecx, 10000h
@@: movq    rdx, xmm15
    cmp     rdx, rsi
    je      @F
    or      ecx, 20000h
@@: mov     eax, ecx
    movdqu  xmm6, [rsp+30h]
    movdqu  xmm7, [rsp+40h]
    movdqu  xmm8, [rsp+50h]
    movdqu  xmm9, [rsp+60h]
    movdqu  xmm10, [rsp+70h]
    movdqu  xmm11, [rsp+80h]
    movdqu  xmm12, [rsp+90h]
    movdqu  xmm13, [rsp+0A0h]
    movdqu  xmm14, [rsp+0B0h]
    movdqu  xmm15, [rsp+0C0h]
    add     rsp, 0D8h
    pop     r15
    pop     r14
    pop     r13
    pop     r12
    pop     rbp
    pop     rdi
    pop     rsi
    pop     rbx
    ret
probe_callout_regs ENDP

; long long probe_fault_offstack(void *new_rsp): switches rsp to new_rsp and executes ud2 there, like
; packed Linux code running on a stack outside the Windows thread stack. The function has unwind
; data with no unwind codes that names probe_lang_handler (as pack.py's static .pdata would), so the
; dispatcher calls that handler for the faulting frame. Returns 13 after the handler resumes it.
probe_fault_offstack PROC FRAME:probe_lang_handler
    .endprolog
    xchg    rsp, rcx
    ud2
    xchg    rsp, rcx
    mov     eax, 13
    ret
probe_fault_offstack ENDP

; the same without a handler in the unwind data (a leaf without unwind data)
probe_fault_offstack_nohandler PROC
    xchg    rsp, rcx
    ud2
    xchg    rsp, rcx
    mov     eax, 14
    ret
probe_fault_offstack_nohandler ENDP

_TEXT ENDS

; A prepared region inside the signed image: 64 KiB of int3 in an executable, read-only section.
GXCODE SEGMENT ALIGN(4096) READ EXECUTE ALIAS(".gxcode") 'CODE'
PUBLIC gx_code_region
gx_code_region DB 65536 DUP (0CCh)
GXCODE ENDS

END
