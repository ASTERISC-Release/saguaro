/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2023 Intel Corporation */

/*
 * aarch64 counterpart of vm/vm_bootloader.S (and, by elimination, of vm/bios/bios.S too).
 *
 * On x86 the kernel entry story is two-stage: QEMU loads a 16-bit BIOS via "-device loader",
 * the BIOS switches to long mode and DMA-loads the `pal` binary through fw_cfg, and
 * vm_bootloader.S (linked into `pal` as .resetvec) flips paging. On QEMU virt none of that
 * exists: QEMU's generic ELF loader maps our PT_LOADs and jumps straight to e_entry at EL1
 * with the MMU and caches off and x0 = physical address of the DTB. So this single file is the
 * whole boot trampoline: enable the MMU with an identity map of the QEMU-virt address space,
 * set up the boot stack and early per-CPU register, and call pal_start_c(dtb).
 *
 * Initial page tables (identity, 4KB granules, 39-bit VA walk starting at Level 1):
 *   L1 covers 512GB in 1GB blocks; index 0 (0x00000000-0x3fffffff: all QEMU-virt device MMIO
 *   -- GIC, PL011, fw_cfg, virtio-mmio, PCIe MMIO window, ECAM) points at an L2 table of
 *   32MB blocks with the device/normal split; indices 1.. map DRAM (from 0x40000000) as
 *   normal WB memory. Memory types via MAIR: attr0 = normal WBWA, attr1 = Device-nGnRE.
 */

    .macro globl sym
    .globl \sym
    .hidden \sym
    .endm

    .section .text.boot, "ax"
    .balign 4

/* ELF entry point; QEMU virt arrives here with x0 = DTB, EL1, MMU off */
    .globl _start
_start:
    msr     daifset, #0xf             /* mask everything while the machine is half-built */

    mov     x20, x0                   /* DTB pointer, survives into pal_start_c's x0 */

    /* boot stack (from .bss, fine with the MMU off: identity physical == link address) */
    adrp    x1, _initial_stack_top
    add     x1, x1, :lo12:_initial_stack_top
    mov     sp, x1

    /* MAIR_EL1: attr0 = Normal WBWA (0xff), attr1 = Device-nGnRE (0x00),
     * attr2 = 0x44 (reserved for the future Write-Combining pages) */
    ldr     x2, =0x00000000004400ff
    msr     mair_el1, x2

    /* TCR_EL1: T0SZ = 25 (39-bit VA, walk starts at L1), 4KB granule, IPS = 0b010 (40-bit
     * PA, as QEMU virt implements). IRGN0/ORGN0/SH0 are written to their architecturally
     * correct values (Normal WBWA, inner-shareable: SH0 [13:12] = 0b11, *not* the reserved
     * 0b01) even though QEMU's TCG ignores them. (Historical note: our first bring-up faults
     * were NOT caused by these fields -- see the block-descriptor SH-position warning in
     * initial_l2_low below; TCR [33:32] is SH0 but a *descriptor's* [33:32] is address.) */
    mov     x2, #25
    orr     x2, x2, #(1 << 8)         /* IRGN0 = Normal WBWA (0b00 would be Device) */
    orr     x2, x2, #(1 << 10)        /* ORGN0 = Normal WBWA */
    orr     x2, x2, #(3 << 12)        /* SH0 = inner shareable (0b11; 0b01 is reserved!) */
    mov     x3, #2
    lsl     x3, x3, #32
    orr     x2, x2, x3                /* IPS = 40 bits */
    msr     tcr_el1, x2

    adrp    x1, initial_l1_table
    add     x1, x1, :lo12:initial_l1_table
    msr     ttbr0_el1, x1
    isb

    /* cold-boot invalidate so enabling the caches cannot resurrect stale lines */
    tlbi    vmalle1
    dsb     ish
    isb

    mrs     x2, sctlr_el1
    /* M (bit 0, MMU) | C (bit 2, D-cache enable) | I (bit 12, I-cache enable). Note that bit 1
     * is A (strict alignment-check enable), *not* C as an earlier seven-bit-mask revision
     * (whose comment mislabeled bits 0,1,2 as "M, C, I") claimed: with A set, every unaligned
     * access to packed data (first victim was virtq_create's `stur wzr, [x20, #2]` on a slab
     * page) took an EC=0x25/DFSC=0x01 alignment fault even with STUR and Normal-WB memory.
     * Clear A defensively instead of relying on the reset value. */
    bic     x2, x2, #2
    orr     x2, x2, #1                /* M */
    orr     x2, x2, #4                /* C */
    orr     x2, x2, #0x1000           /* I */
    msr     sctlr_el1, x2
    isb

    /* CPACR_EL1.FPEN (bits [21:20]) = 0b11: allow FPSIMD at EL1&EL0 without trapping.
     * QEMU resets FPEN to 0b00, so the first `str q0` inside any variadic pal_log() would
     * take an EC=0 CPTR trap; the arm64 Linux head.S does the exact same enable here.
     * (Also scrub FPCR/FPSR so no boot-time FP state depends on reset luck.) */
    mrs     x1, cpacr_el1
    orr     x1, x1, #(3 << 20)
    msr     cpacr_el1, x1
    isb
    msr     fpcr, xzr
    msr     fpsr, xzr

    /* zero the user TLS register (TPIDR_EL0 belongs to the guest app later; the kernel must
     * never leave boot junk there -- see the cur_tcb design in kernel_multicore.h) */
    msr     tpidr_el0, xzr

    /* early per-CPU slot: pal_start_c() upgrades TPIDR_EL1 to the allocated array in
     * init_multicore_prepare(); everything before that uses this static instance */
    adrp    x1, g_early_per_cpu_data
    add     x1, x1, :lo12:g_early_per_cpu_data
    msr     tpidr_el1, x1

    mov     x29, xzr
    mov     x30, xzr
    mov     x0, x20                   /* pal_start_c(const void* fdt) */
    bl      pal_start_c

    /* noreturn; a return from the C kernel is a fatal bug */
1:  b       1b

/* ---------------------------------------------------------------------------
 * Static initial page tables (identity map). Link-time values are safe because
 * the kernel is linked at its load address (0x40080000) and the whole point of
 * the boot map is 1:1 physical == virtual translation.
 * --------------------------------------------------------------------------- */
    .section .data, "aw"

/* L2 table for the first 1GB: 2MB blocks covering the whole QEMU virt device world
 * (512 entries exactly fill the page). Block-descriptor bits: type(01) | AttrIndx [4:2]
 * (Device = 1 -> +0x4) | SH [9:8] (inner = 0b11 -> +0x300) | AF [10] (+0x400)
 * => normal 0x701, device 0x705.
 * WARNING: do NOT set bits [33:32] here -- that is the *TCR* SH0 position; in a block
 * descriptor those bits lie INSIDE the output address and would translate every mapping
 * to PA 0x3_0000_0000+addr (unassigned -> "sync external abort on translation walk"). */
    .balign 4096
initial_l2_low:
    /* [0x00000000, 0x08000000): flash alias, nothing attached; normal */
1:  .rept   64                            /* 64 x 2MB */
    .quad   (. - 1b) / 8 * 0x00200000 + 0x701
    .endr
    /* [0x08000000, 0x0c000000): GIC dist/redist (0x08..), PL011 (0x09000000),
     * fw_cfg (0x09020000), virtio-mmio window (0x0a000000-) -- all device */
    .rept   32
    .quad   (. - 1b) / 8 * 0x00200000 + 0x705
    .endr
    /* [0x0c000000, 0x10000000): nothing attached; normal */
    .rept   32
    .quad   (. - 1b) / 8 * 0x00200000 + 0x701
    .endr
    /* [0x10000000, 0x40000000): PCIe MMIO window + ECAM at 0x3f000000 -- device */
    .rept   384
    .quad   (. - 1b) / 8 * 0x00200000 + 0x705
    .endr

/* L1 table: walk base for T0SZ = 25. Entry 0 -> the device L2 above; entries 1.. describe
 * DRAM from 0x40000000 upward as normal WB (extra entries beyond installed RAM are harmless
 * translations that the DTB-sized memory map never touches).
 * NOTE: label `2` marks entry 0 so that `(. - 2b) / 8` is the true L1 index for every entry
 * emitted by .rept below (the explicit L1[0] .quad keeps it at index 0 == base + 0).
 * 0x701 = block | AttrIndx 0 (normal WBWA) | SH inner [9:8] | AF [10]; 1GB blocks carry
 * their OA from bit 30, so the descriptor format notes in initial_l2_low apply verbatim. */
    .balign 4096
2:  initial_l1_table:
    .quad   initial_l2_low + 3        /* table descriptor */
    .rept   9                         /* 1GB blocks [0x40000000 .. 0x280000000) */
    .quad   (. - 2b) / 8 * 0x40000000 + 0x701
    .endr
    .space  4096 - 10 * 8, 0          /* remaining L1 entries invalid */

    .section .bss, "aw"
    .balign 16
    .space  0x4000                    /* 16KB boot stack */
_initial_stack_top:
