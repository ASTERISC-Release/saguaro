/* Text segment (with the ELF and program headers) at 0, data segment at DATA_ADDR (given by
   build.sh: the distance between the two sections in the linked enclave image). */
PHDRS { text PT_LOAD FILEHDR PHDRS FLAGS(5); data PT_LOAD FLAGS(6); }
ENTRY(_start)
SECTIONS {
    . = SIZEOF_HEADERS;
    .text : { *(.text*) } :text
    .rodata : { *(.rodata*) } :text
    . = DATA_ADDR;
    .data : { *(.data*) } :data
    .bss : { *(.bss*) *(COMMON) } :data
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) *(.interp) *(.dynamic) *(.dynsym) *(.dynstr) *(.hash) *(.gnu.hash) }
}
