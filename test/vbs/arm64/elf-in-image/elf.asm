; Saguaro phase-0 spike (step h): the two segments of the Linux program (linux/app.elf), placed
; into the enclave image at build time: the text segment (ELF headers, .text, .rodata) in an
; executable read-only section, the data segment (.data, .bss) in a writable section that the
; linker is expected to place directly after it (checked at run time).

    AREA |.gxelft|, CODE, READONLY, ALIGN=12
    EXPORT gx_elf_text
gx_elf_text
    INCBIN text.bin

    AREA |.gxelfd|, DATA, READWRITE, ALIGN=12
    EXPORT gx_elf_data
gx_elf_data
    INCBIN data.bin

    AREA |.text|, CODE, READONLY, ALIGN=4
    EXPORT probe_jump
    EXPORT probe_read_x18
; long probe_jump(void *fn, long a0, long a1): tail-calls fn(a0, a1) without a CFG check
probe_jump PROC
    mov     x16, x0
    mov     x0, x1
    mov     x1, x2
    br      x16
    ENDP

probe_read_x18 PROC
    mov     x0, x18
    ret
    ENDP
    END
