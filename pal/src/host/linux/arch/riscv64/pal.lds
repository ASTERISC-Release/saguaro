OUTPUT_FORMAT("elf64-littleriscv", "elf64-littleriscv", "elf64-littleriscv")
OUTPUT_ARCH(riscv)
ENTRY(pal_start);

SECTIONS
{
  /* Read-only sections, merged into text segment; */
  __text_start = .;
  . = SEGMENT_START("text-segment", 0) + SIZEOF_HEADERS;
  .note.gnu.build-id : { *(.note.gnu.build-id) }
  .hash          : { *(.hash) }
  .gnu.hash      : { *(.gnu.hash) }
  .dynsym        : { *(.dynsym) }
  .dynstr        : { *(.dynstr) }
  .gnu.version   : { *(.gnu.version) }
  .gnu.version_d : { *(.gnu.version_d) }
  .gnu.version_r : { *(.gnu.version_r) }
  .rela.dyn       :
     {
      *(.rela.init)
      *(.rela.text .rela.text.* .rela.gnu.linkonce.t.*)
      *(.rela.fini)
      *(.rela.rodata .rela.rodata.* .rela.gnu.linkonce.r.*)
      *(.rela.data .rela.data.* .rela.gnu.linkonce.d.*)
      *(.rela.tdata .rela.tdata.* .rela.gnu.linkonce.td.*)
      *(.rela.tbss .rela.tbss.* .rela.gnu.linkonce.tb.*)
      *(.rela.ctors)
      *(.rela.dtors)
      *(.rela.got)
      *(.rela.bss .rela.bss.* .rela.gnu.linkonce.b.*)
      *(.rela.ldata .rela.ldata.* .rela.gnu.linkonce.l.*)
      *(.rela.lbss .rela.lbss.* .rela.gnu.linkonce.lb.*)
      *(.rela.lrodata .rela.lrodata.* .rela.gnu.linkonce.lr.*)
      *(.rela.ifunc)
    }
  .rela.plt      :
    {
      *(.rela.plt)
      *(.rela.iplt)
    }
  .init          : { *(.init) }
  .plt           : { *(.plt) *(.iplt) }
  .text :
  {
    /* the rest of text segments */
    *(.text .stub .text.*);
  }
  .fini          : { *(.fini) }
  .rodata        :
  {
    /* the rest of rodata */
    *(.rodata .rodata.*)
  }
  .eh_frame_hdr  : { *(.eh_frame_hdr) }
  .eh_frame      : ONLY_IF_RO { *(.eh_frame) }
  __text_end = .;

  /* now adjust the address for the data segment */
  . = DATA_SEGMENT_ALIGN (CONSTANT (MAXPAGESIZE), CONSTANT (COMMONPAGESIZE));
  __data_start = .;
  .eh_frame      : ONLY_IF_RW { *(.eh_frame) }
  .data.rel.ro   : { *(.data.rel.ro) }
  .ctors         : { *(.ctors) }
  .dtors         : { *(.dtors) }
  .jcr           : { *(.jcr) }
  .dynamic       : { *(.dynamic) }
  .got           : { *(.got) *(.igot) }
  .got.plt       : { *(.got.plt) *(.igot.plt) }
  .init_array    : {
    __init_array_start = .;
    KEEP (*(SORT_BY_INIT_PRIORITY(.init_array.*) .init_array))
    __init_array_end = .;
  }
  . = DATA_SEGMENT_RELRO_END (0, .);
  .data :
  {
    /* the rest of data segment */
    *(.data .data.*)
    /* RISC-V small data (<=8-byte initialized objects, placed by GCC in .sdata/.sdata1/
     * .sdata2 for gp-relative addressing; the aarch64/x86_64 siblings have no such classes and
     * the `*(.data .data.*)` glob above does not match ".sdata*"). Neither libpal.so nor
     * libsysdb.so is ever gp-relaxed (both are ET_DYN with all data preemptible -- see
     * Documentation/devel/riscv/PLAN.md "Linker relaxation"), so these are plain .data-adjacent
     * storage here, but they must be *placed*: an orphan section's location is ld's choice. */
    *(.sdata .sdata.*)
  }
  .bss            :
  {
    *(.bss .bss.*)
    /* RISC-V small BSS (see the .sdata comment above; orphan NOBITS placement would also find
     * .bss eventually, we just do not want to depend on that here). */
    *(.sbss .sbss.* .scommon)
    *(COMMON)
  }
  . = DATA_SEGMENT_END(.);
  __data_end = .;
}
