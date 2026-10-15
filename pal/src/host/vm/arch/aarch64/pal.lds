/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2023 Intel Corporation */

/*
 * aarch64 counterpart of pal/src/host/vm/pal.lds.
 *
 * Same three-segment layout (ro/rw/rx) as the x86 kernel so that pal_rtld's generic PT_DYNAMIC
 * verification works unchanged, but:
 *   - load address is 0x40080000: QEMU virt's DRAM starts at 0x40000000 and the conventional
 *     -kernel load offset is +0x80000 (the arm Linux kernel's own Text Offset; QEMU's generic
 *     ELF loader honors the ELF's own addresses, this offset just keeps us clear of the DTB
 *     that QEMU places at the very start of DRAM).
 *   - no 64KB resetvec alignment convention (that is an x86 BIOS thing); instead the entry is
 *     `.text.boot` at the head of the rx segment, entered via the ELF e_entry.
 *   - no PT_PHDR/FILEHDR gymnastics beyond what keeps pal_rtld's checks satisfied.
 */

OUTPUT_FORMAT("elf64-littleaarch64", "elf64-littleaarch64", "elf64-littleaarch64")
OUTPUT_ARCH(aarch64)

ENTRY(_start);

PHDRS
{
  /* No FILEHDR/PHDRS/PT_PHDR here unlike x86: QEMU's *aarch64* ELF loader refuses any PT_LOAD
   * that maps the file header (p_offset < sizeof(Ehdr)) with "incompatible architecture" — that
   * exemption is x86-only — so the kernel must not cover the header in any segment. Nothing in
   * PAL reads PT_PHDR/DT_PHDR (pal_rtld locates .dynamic by section and the phdrs via
   * __ehdr_start), and the x86 linux-kernel-boot path that tolerates it is irrelevant here. */
  ro PT_LOAD FLAGS(4);
  rw PT_LOAD FLAGS(6);
  rx PT_LOAD FLAGS(5);
  dyn PT_DYNAMIC;
  note PT_NOTE;
}

SECTIONS
{
  /* QEMU virt: first DRAM bank at 0x40000000; the kernel loads at the kernel-image-protocol
     conventional offset 0x80000 into it (also clears the DTB placed at 0x40000000). */
  . = 0x40080000;

  /* See setup_pal_binary()'s aarch64+HOST_TYPE branch in pal_rtld.c: because QEMU's aarch64
     ELF loader refuses header-mapping PT_LOAD segments, the headers are not in memory, so
     pal_rtld takes the image base and .dynamic address from these link-time symbols instead
     of walking PT_PHDR/PT_DYNAMIC. */
  PROVIDE_HIDDEN (__pal_image_base = .);
  PROVIDE_HIDDEN (__fw_base = .);
  PROVIDE_HIDDEN (__data_start = .);

  . += SIZEOF_HEADERS;

  /* Read-only (ro) segment */
  .note.gnu.build-id : { *(.note.gnu.build-id) } :ro :note
  .note              : { *(.note .note.*) } :ro :note
  .hash              : { *(.hash) } :ro
  .gnu.hash          : { *(.gnu.hash) } :ro
  .dynsym            : { *(.dynsym) } :ro
  .dynstr            : { *(.dynstr) } :ro
  .gnu.version       : { *(.gnu.version) } :ro
  .gnu.version_d     : { *(.gnu.version_d) } :ro
  .gnu.version_r     : { *(.gnu.version_r) } :ro
  .rela.dyn          :
     {
      *(.rela.init)
      *(.rela.text .rela.text.* .rela.gnu.linkonce.t.*)
      *(.rela.fini)
      *(.rela.rodata .rela.rodata.* .rela.gnu.linkonce.r.*)
      *(.rela.data .rela.data.*)
      *(.rela.tdata .rela.tdata.*)
      *(.rela.tbss .rela.tbss.*)
      *(.rela.ctors)
      *(.rela.dtors)
      *(.rela.got)
      *(.rela.bss .rela.bss.*)
      *(.rela.ldata .rela.ldata.*)
      *(.rela.lbss .rela.lbss.*)
      *(.rela.lrodata .rela.lrodata.*)
      *(.rela.ifunc)
      *(.rela.plt)
    } :ro
  .rodata            : { *(.rodata .rodata.* .gnu.linkonce.r.*) } :ro
  .eh_frame_hdr      : { *(.eh_frame_hdr) } :ro
  .eh_frame          : ONLY_IF_RO { *(.eh_frame) } :ro

  /* Read-writable (rw) segment */
  . = ALIGN(CONSTANT (MAXPAGESIZE));
  .data.rel.ro   : { *(.data.rel.ro) }
  .eh_frame      : ONLY_IF_RW { *(.eh_frame) } :rw
  .ctors         : { *(.ctors) } :rw
  .dtors         : { *(.dtors) } :rw
  .dynamic       : { PROVIDE_HIDDEN (__pal_dynamic_start = .); *(.dynamic) } :rw :dyn
  .got           : { *(.got) *(.igot) } :rw
  .got.plt       : { *(.got.plt) *(.igot.plt) } :rw
  .init_array    : {
    __init_array_start = .;
    KEEP (*(SORT_BY_INIT_PRIORITY(.init_array.*) .init_array))
    __init_array_end = .;
  } :rw
  .data          : { *(.data .data.*) } :rw
  /* PAL state private to each process: the multiprocess support copies these pages into every
     process root (pal_internal.h, PAL_PROCESS_DATA). Before .bss: a PROGBITS section after the
     NOBITS .bss (with its 64 MB shared region) would put all of .bss into the file. */
  . = ALIGN(4096);
  PROVIDE_HIDDEN (__process_data_start = .);
  .process_data  : { *(.process_data) } :rw
  . = ALIGN(4096);
  PROVIDE_HIDDEN (__process_data_end = .);

  .bss           : { *(.bss .bss.*) *(COMMON) } :rw

  PROVIDE_HIDDEN (__data_end = .);

  /* Executable (rx) segment; .text.boot first so the entry point is at the segment head */
  . = ALIGN(CONSTANT (MAXPAGESIZE));
  PROVIDE_HIDDEN (__text_start = .);
  .text.boot     : { KEEP (*(.text.boot)) } :rx
  .init          : { *(.init) } :rx
  .fini          : { *(.fini) } :rx
  .plt           : { *(.plt) *(.iplt) } :rx
  .text          : { *(.text .stub .text.*) } :rx

  PROVIDE_HIDDEN (__text_end = .);
}
