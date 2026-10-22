; Saguaro phase-0 spike: a section that the linker is asked to mark read+write+execute
; (build-rwx.cmd links it with /SECTION:.gxrwx,ERW) to see what /ENCLAVE and the enclave loader do.
    AREA |.gxrwx|, CODE, READWRITE, ALIGN=12
    EXPORT gx_rwx_region
gx_rwx_region
    DCD     0xd2800540      ; mov x0, #42
    DCD     0xd65f03c0      ; ret
    SPACE   4088
    END
