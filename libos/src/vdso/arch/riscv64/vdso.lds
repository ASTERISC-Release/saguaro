/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2018 Intel Corporation
 *                    Isaku Yamahata <isaku.yamahata at gmail.com>
 *                                   <isaku.yamahata at intel.com>
 */

/*
 * Linker script for Gramine vDSO emulation
 *
 * The .text fill pattern below (0x00000013) is the 4-byte RISC-V base-ISA NOP encoding (`addi
 * zero, zero, 0`), used to pad any gap in the .text section -- x86_64's version uses a repeated
 * 1-byte NOP (0x90) since x86 NOPs are single bytes; riscv64 base instructions are always fixed
 * 4-byte width (compressed `c.nop` only exists *inside* the .text itself, never as a fill
 * pattern), so the fill unit here has to be the whole instruction, not a byte pattern tiled
 * four times.
 *
 * No time;/__vdso_time; exports here: riscv64 has no __NR_time syscall number at all (see
 * vdso.c/vdso.h), so there is nothing to export under that name.
 */

PHDRS
{
        text            PT_LOAD         FLAGS(5) FILEHDR PHDRS;
        dynamic         PT_DYNAMIC      FLAGS(4);
        note            PT_NOTE         FLAGS(4);
        eh_frame_hdr    PT_GNU_EH_FRAME;
}

SECTIONS
{
        . = SIZEOF_HEADERS;
        .hash : { *(.hash) } :text
        .gnu.hash : { *(.gnu.hash) }
        .dynsym : { *(.dynsym) }
        .dynstr : { *(.dynstr) }
        .gnu.version : { *(.gnu.version) }
        .gnu.version_d : { *(.gnu.version_d) }
        .gnu.version_r : { *(.gnu.version_r) }
        .dynamic : { *(.dynamic) } :text :dynamic
        .rodata : {
                *(.rodata*)
                *(.data*)
                *(.sdata*)
                *(.got.plt) *(.got)
                *(.gnu.linkonce.d.*)
                *(.bss*)
                *(.dynbss*)
                *(.gnu.linkonce.b.*)
        } : text
        .note : { *(.note.*) } :text :note
        .eh_frame_hdr : { *(.eh_frame_hdr) } :text :eh_frame_hdr
        .eh_frame : { KEEP (*(.eh_frame)) } :text
        .text   : { *(.text*) } : text = 0x00000013,

        /DISCARD/ : {
                *(.discard)
                *(.discard.*)
        }
}

VERSION {
        LINUX_2.6 {
        global:
                clock_gettime;
                __vdso_clock_gettime;
                gettimeofday;
                __vdso_gettimeofday;
                getcpu;
                __vdso_getcpu;

                __vdso_libos_syscall_entry;
        local: *;
        };
}
