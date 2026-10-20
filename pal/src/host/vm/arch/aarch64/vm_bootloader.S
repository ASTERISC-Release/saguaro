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

#include "asm-offsets.h"    /* PER_CPU_DATA_* consumed by the pal_ap_entry trampoline */

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
    /* UCT (bit 15): let EL0 read CTR_EL0 (cache line sizes; nginx and libgcc's __clear_cache do);
     * UCI (bit 26): let EL0 do DC CVAU/CIVAC/CVAC and IC IVAU (JITs, __clear_cache). Linux sets both
     * for user space; without them each EL0 `mrs x, ctr_el0` is an unhandled EC=0x18 exception that
     * kills the guest. */
    orr     x2, x2, #0x8000           /* UCT */
    orr     x2, x2, #0x4000000        /* UCI */

    /* Clear PSTATE.PAN (ARMv8.1; QEMU -cpu max boots EL1 with PAN set, as the architecture
     * mandates) *before* sctlr_el1/M above: with PAN=1, QEMU's walker zeroes the EL1
     * permissions of every page that is also EL0-rw (get_S1prot's `if (user_rw &&
     * regime_is_pan(...)) prot_rw = 0`) -- and although the intent is to gate *data*
     * accesses, result->f.prot is what the instruction-fetch path also checks for
     * PAGE_EXEC. The kernel's DRAM blocks are EL0-rw (unikernel design), so with PAN still
     * set the very first translated fetch after the MMU comes up takes a level-1 permission
     * fault (observed: EC=0x21/FSC=0x0d abort at the `isb` right after `msr sctlr_el1`).
     * PAN has no policy role here -- the kernel accesses user memory directly, exactly the
     * `msr pan, #0` Linux's head.S performs for its identity stage. */
    .arch_extension pan
    msr     pan, #0

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

    /* CNTKCTL_EL1.EL0VCTEN (bit 1): EL0 may read CNTVCT_EL0, as on Linux (applications and
     * their libraries read the virtual counter directly, e.g. OpenSSL's timing code; with the
     * bit clear the read traps as EC 0x18 and the guest panics). Per-CPU, so pal_ap_entry does
     * the same; the other bits (EL0PCTEN, the event stream, EL0 timer access) stay as they are. */
    mrs     x1, cntkctl_el1
    orr     x1, x1, #(1 << 1)
    msr     cntkctl_el1, x1
    isb

    /* zero the user TLS register (TPIDR_EL0 belongs to the guest app later; the kernel must
     * never leave boot junk there -- see the cur_tcb design in kernel_multicore.h) */
    msr     tpidr_el0, xzr

    /* early per-CPU slot: pal_start_c() upgrades TPIDR_EL1 to the allocated array in
     * init_multicore_prepare(); everything before that uses this static instance */
    adrp    x1, g_early_per_cpu_data
    add     x1, x1, :lo12:g_early_per_cpu_data
    msr     tpidr_el1, x1

    /* TPIDRRO_EL0 (EL0-read-only) = &this CPU's cur_tcb cell, so EL0's pal_get_tcb() has a
     * per-CPU "which thread am I" answer from the very first instruction that runs at EL0
     * (Linux vDSO uses the same register for the cpu id). Never rewritten by switches: the
     * cell holds the *current* thread, per CPU. Upgraded in init_multicore_prepare(). */
    add     x1, x1, #PER_CPU_DATA_CUR_TCB
    msr     tpidrro_el0, x1

    /* VBAR_EL1 now, as pal_ap_entry does on the secondary CPUs (syscalls_init() writes the same
     * value later): the generic timer is armed in gic_init(), before syscalls_init(), and any
     * spinlock_unlock_enable_irq() of the early boot unmasks IRQs, so a tick in that window would
     * otherwise be taken through the reset VBAR (0, the flash alias) and hang the boot (the x86_64
     * boot flake of dev docs: vm-multiproc/FINDINGS.md had the same shape with the IDT) */
    adrp    x1, vectors
    add     x1, x1, :lo12:vectors
    msr     vbar_el1, x1
    isb

    mov     x29, xzr
    mov     x30, xzr
    mov     x0, x20                   /* pal_start_c(const void* fdt) */
    bl      pal_start_c

    /* noreturn; a return from the C kernel is a fatal bug */
1:  b       1b

/* ---------------------------------------------------------------------------
 * PSCI CPU_ON entry point for secondary CPUs (vm-common/arch/aarch64/kernel_multicore.c
 * issues the calls). QEMU's in-kernel PSCI firmware starts the vCPU at EL1 at the
 * entry-point address with x0 = the ContextId we passed (= cpu index), MMU and caches
 * off, and no other state worth trusting -- so this mirrors _start's per-CPU bring-up
 * (the sysregs below are per-CPU copies; the page tables themselves are shared
 * identity-mapped .data) and then hands over to pal_start_ap_c(). The BSP-only parts
 * (DTB parse, cmdline, relocation) are intentionally absent; DAIF stays masked just
 * like on the BSP until interrupts_init() runs in the C bring-up.
 *
 * Keep in lockstep with _start: any sysreg _start programs per-CPU must appear here
 * too, or the AP runs with reset values behind the BSP's back.
 * --------------------------------------------------------------------------- */
    globl pal_ap_entry
pal_ap_entry:
    msr     daifset, #0xf
    /* x0 == cpu index (the PSCI ContextId). CPU_ON is only ever issued for indices
     * >= 1; index 0 would mean the BSP itself got powered back on -- park it rather
     * than double-boot the kernel on top of its own live state. */
    cbz     x0, 2f

    /* this CPU's slot: g_per_cpu_data + cpu_idx * sizeof(struct per_cpu_data) */
    adrp    x1, g_per_cpu_data
    add     x1, x1, :lo12:g_per_cpu_data
    ldr     x1, [x1]
    ldr     x2, =PER_CPU_DATA_SIZE
    madd    x3, x0, x2, x1
    mov     x20, x0                     /* survives the sysreg sequence for the C call */
    msr     tpidr_el1, x3               /* per-CPU data singleton, set once and forever */
    /* EL0-readable TCB cell for this CPU, same contract as _start installs for the BSP */
    add     x4, x3, #PER_CPU_DATA_CUR_TCB
    msr     tpidrro_el0, x4
    ldr     x2, [x3, #PER_CPU_DATA_INTERRUPT_STACK]
    mov     sp, x2

    ldr     x2, =0x00000000004400ff     /* MAIR: same attr0/attr1/attr2 as _start */
    msr     mair_el1, x2
    /* TCR: identical fields to _start's step-by-step build (T0SZ=25, WBWA, SH0=0b11,
     * IPS=40-bit) -- spelled as the assembled constant because this is a hot,
     * correctness-critical boot path on a CPU that has nothing configured yet. */
    ldr     x2, =(25 | (1 << 8) | (1 << 10) | (3 << 12))
    mov     x3, #2
    lsl     x3, x3, #32
    orr     x2, x2, x3
    /* the TTBR1 half, as memory_pagetables_init() set it on the BSP (TCR_T1_FIELDS there):
     * T1SZ = 25, IRGN1/ORGN1 = WBWA, SH1 = inner, TG1 = 4 KB; TTBR1 = the frame alias table */
    ldr     x3, =((25 << 16) | (1 << 24) | (1 << 26) | (3 << 28) | (2 << 30))
    orr     x2, x2, x3
    msr     tcr_el1, x2
    adrp    x2, g_ttbr1_root
    add     x2, x2, :lo12:g_ttbr1_root
    ldr     x2, [x2]
    msr     ttbr1_el1, x2
    /* TTBR0 := the *live* root table, NOT this file's initial_l1_table.
     * memory_pagetables_init() (pal_start_c, long before PSCI CPU_ON) replaced these coarse
     * boot tables -- whose DRAM entries are 1GB blocks mapped EL1-only -- with the fine
     * tables that carry the EL0-accessible leaves (PTE_DATA/PTE_EXEC) the kernel installs
     * for libsysdb and the app. An AP installed on initial_l1_table therefore runs its EL1
     * kernel code fine but permission-faults at walk level 1 (DFSC 0x0d) on the *first EL0
     * access* of any scheduler-migrated thread there: with per-CPU TLBIs and shootdown now
     * in place, every CPU must walk the one root the kernel mutates. init_multicore()
     * asserts g_page_table_root is non-NULL before issuing any CPU_ON. */
    adrp    x2, g_page_table_root
    add     x2, x2, :lo12:g_page_table_root
    ldr     x2, [x2]
    msr     ttbr0_el1, x2
    isb
    tlbi    vmalle1
    dsb     ish
    isb

    mrs     x2, sctlr_el1
    bic     x2, x2, #2                  /* A: alignment checks off, same as _start */
    orr     x2, x2, #1                  /* M */
    orr     x2, x2, #4                  /* C */
    orr     x2, x2, #0x1000             /* I */
    orr     x2, x2, #0x8000             /* UCT: EL0 may read CTR_EL0, same as _start */
    orr     x2, x2, #0x4000000          /* UCI: EL0 may do DC/IC maintenance, same as _start */
    /* PSTATE.PAN is per-CPU and QEMU -cpu max resets it set on every CPU, not just the
     * BSP -- same clear-before-SCTLR ordering (and same reason) as _start. */
    msr     pan, #0
    msr     sctlr_el1, x2
    isb

    mrs     x2, cpacr_el1
    orr     x2, x2, #(3 << 20)          /* CPACR_EL1.FPEN = 0b11 */
    msr     cpacr_el1, x2
    isb
    mrs     x2, cntkctl_el1
    orr     x2, x2, #(1 << 1)           /* CNTKCTL_EL1.EL0VCTEN, as on the boot CPU */
    msr     cntkctl_el1, x2
    isb
    msr     fpcr, xzr
    msr     fpsr, xzr
    msr     tpidr_el0, xzr

    /* VBAR_EL1 is per-CPU too: install the table before unmasking anything so a stray
     * fault on this CPU cannot take QEMU's reset vector (syscalls_init() rewrites the
     * same value during the C bring-up; same identity-mapped label, harmless). */
    adrp    x2, vectors
    add     x2, x2, :lo12:vectors
    msr     vbar_el1, x2
    isb

    mov     x0, x20
    bl      pal_start_ap_c
2:  wfe
    b       2f                          /* pal_start_ap_c is noreturn; never reached */

/* ---------------------------------------------------------------------------
 * Static initial page tables (identity map). Link-time values are safe because
 * the kernel is linked at its load address (0x40080000) and the whole point of
 * the boot map is 1:1 physical == virtual translation.
 * --------------------------------------------------------------------------- */
    .section .data, "aw"

/* L2 table for the first 1GB: 2MB blocks covering the whole QEMU virt device world
 * (512 entries exactly fill the page). Block-descriptor bits (same layout at every level,
 * matching Linux's PMD_* macros and QEMU's walker): valid[0] (L1/L2 blocks are bit[1] == 0,
 * like Linux's PMD_TYPE_SECT) | AttrIndx [4:2] (Device = 1 -> +0x4) | AP[2:1] [7:6] (0b00 =
 * EL1-only; 0b01 = EL1 rw + EL0 rw -> +0x40) | SH [9:8] (inner = 0b11 -> +0x300) | AF [10]
 * (+0x400) => normal 0x701, device 0x705. These entries all stay EL1-only: the device world
 * must not be reachable from userland, and the DRAM 1GB blocks below are EL1-only at boot
 * too -- EL0 accessibility is installed per-page by memory_pagetables_init()/B4 (see the L1
 * table's comment for why the coarse AP = 0b01 experiment cannot work).
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
 * 0x701 = block | AttrIndx 0 (normal WBWA) | AP[2:1] = 0b00: EL1-only | SH inner [9:8] |
 * AF [10]. 1GB blocks carry their OA from bit 30, so the descriptor format notes in
 * initial_l2_low apply verbatim.
 * WHY THESE ENTRIES CANNOT SIMPLY BE 0x741 (AP=0b01, "EL1 rw + EL0 rw") -- the naive way to
 * make userland's data accesses work: QEMU's AArch64 S1 permission model (target/arm/ptw.c
 * get_S1prot) implements the two-range EL1&0 regime with `xn = pxn || (user_rw & PAGE_WRITE)`,
 * i.e. an EL1 *instruction fetch* from any page that is EL0-writable permission-faults
 * (observed: EC=0x21/FSC=0x0d Prefetch Abort at the first translated fetch right after
 * `msr sctlr_el1` -- the kernel cannot even boot, and PSTATE.PAN=0 does not help, PAN only
 * gates data accesses). EL0-rw (0b01) + EL1-exec is inexpressible under that rule; the only
 * encodings that give both EL1 and EL0 execute are AP=0b11 (EL1 ro+x, EL0 ro+x) and AP=0b10
 * (EL1 ro+x, EL0 none) -- per-page choices the bootloader's coarse 1GB blocks cannot make.
 * So the coarse map stays EL1-only, the kernel boots on it, and memory_pagetables_init()
 * swaps in VMSA L2/L3 tables (B4) whose exec pages are AP=0b11 (PAL text and LibOS's
 * libos_syscall_entry: EL1 dispatches into that code at syscall time) while data/stack
 * pages become AP=0b01 with PAN kept cleared. */
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
