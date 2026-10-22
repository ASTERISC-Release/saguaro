; Saguaro phase-0 spike: AArch64 helpers for the probe enclave (armasm64 syntax).
; Leaf functions without unwind data; exceptions in them unwind through LR.

    AREA |.text|, CODE, READONLY, ALIGN=4

    EXPORT probe_svc0
    EXPORT probe_brk0
    EXPORT probe_brkf000
    EXPORT probe_udf
    EXPORT probe_read_tpidr
    EXPORT probe_write_tpidr
    EXPORT probe_read_tpidrro
    EXPORT probe_read_x18
    EXPORT probe_read_sp
    EXPORT probe_jump
    EXPORT probe_sync_icache
    EXPORT probe_read_isar0
    EXPORT probe_read_midr
    EXPORT probe_read_cntvct
    EXPORT probe_read_cntfrq
    EXPORT probe_casal

; long probe_svc0(long a0, long a1, long a2, long nr): Linux-style system call: x8 = nr; svc #0
probe_svc0 PROC
    mov     x8, x3
    svc     #0
    ret
    ENDP

probe_brk0 PROC
    brk     #0
    mov     x0, #7
    ret
    ENDP

probe_brkf000 PROC
    brk     #0xf000
    mov     x0, #8
    ret
    ENDP

; udf #0 (permanently undefined, encoding 0x00000000)
probe_udf PROC
    DCD     0x00000000
    mov     x0, #9
    ret
    ENDP

probe_read_tpidr PROC
    mrs     x0, TPIDR_EL0
    ret
    ENDP

probe_write_tpidr PROC
    msr     TPIDR_EL0, x0
    ret
    ENDP

probe_read_tpidrro PROC
    mrs     x0, TPIDRRO_EL0
    ret
    ENDP

probe_read_x18 PROC
    mov     x0, x18
    ret
    ENDP

probe_read_sp PROC
    mov     x0, sp
    ret
    ENDP

; long probe_jump(void *fn, long a0, long a1): tail-calls fn(a0, a1) without a CFG check
probe_jump PROC
    mov     x16, x0
    mov     x0, x1
    mov     x1, x2
    br      x16
    ENDP

; void probe_sync_icache(void *p, size_t len): clean D-cache and invalidate I-cache by 4-byte lines (simple)
probe_sync_icache PROC
    add     x1, x1, x0
    bic     x2, x0, #63
1
    dc      cvau, x2
    add     x2, x2, #64
    cmp     x2, x1
    b.lo    %b1
    dsb     ish
    bic     x2, x0, #63
2
    ic      ivau, x2
    add     x2, x2, #64
    cmp     x2, x1
    b.lo    %b2
    dsb     ish
    isb
    ret
    ENDP

probe_read_isar0 PROC
    mrs     x0, ID_AA64ISAR0_EL1
    ret
    ENDP

probe_read_midr PROC
    mrs     x0, MIDR_EL1
    ret
    ENDP

probe_read_cntvct PROC
    mrs     x0, CNTVCT_EL0
    ret
    ENDP

probe_read_cntfrq PROC
    mrs     x0, CNTFRQ_EL0
    ret
    ENDP

; long probe_casal(long *p, long expected, long desired): LSE compare-and-swap, returns old value
probe_casal PROC
    casal   x1, x2, [x0]
    mov     x0, x1
    ret
    ENDP

; A prepared region inside the signed image: 64 KiB of zeros in an executable, read-only section.
    AREA |.gxcode|, CODE, READONLY, ALIGN=12
    EXPORT gx_code_region
gx_code_region
    SPACE   65536

    END
