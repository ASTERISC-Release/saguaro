/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2023 Intel Corporation */

/*
 * aarch64 counterpart of vm-common/kernel_memory.c: the VMSA (4KB granule) page-table manager.
 *
 * vm_bootloader.S enters the kernel on coarse identity tables (device L2 for [0,1GB), EL1-only
 * 1GB blocks for DRAM) because QEMU's S1 permission model forbids EL1 to *execute* from any page
 * that is EL0-writable (target/arm/ptw.c get_S1prot: `xn = pxn || (user_rw & PAGE_WRITE)` for the
 * two-range EL1&0 regime; see the long comment in vm_bootloader.S). `memory_pagetables_init()`
 * below builds the real L1/L2/L3 hierarchy -- executable segments with AP=0b11 (EL1 r+x and EL0
 * r+x, the only encoding satisfying both the kernel's own fetches, pal_* calls from EL0, and the
 * EL1-dispatch into LibOS's libos_syscall_entry), data/stacks with AP=0b01 (EL1 rw + EL0 rw,
 * which requires the `msr pan, #0` the bootloader performs), and the device world kept EL1-only
 * -- and swaps it into TTBR0_EL1. Everything stays identity-mapped (VA == PA), like x86's kernel
 * keeps its identity boot map under its own CR3.
 *
 * The x86 file's PAT/WC machinery maps to MAIR attribute indices here: attr0 = Normal WBWA,
 * attr1 = Device-nGnRE (both installed by the bootloader); the Write-Combining marks stay no-ops
 * until a consumer needs them (graphics forwarding is out of scope for the arm64 port).
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "api.h"
#include "pal_error.h"
#include "pal_internal.h"
#include "spinlock.h"

#include "kernel_dtb.h"
#include "kernel_interrupts.h"
#include "kernel_memory.h"
#include "kernel_multicore.h"

/* see kernel_dtb.h: QEMU virt DRAM base (fallback until /memory parsing lands) */
#define ARM_KERNEL_RESERVE_SIZE (32UL * 1024 * 1024) /* kernel binary + initial tables */

/* Region from which "shared" (device-visible) memory is carved. On x86 this is the pre-reserved
 * [658MB, 896MB] window; on arm the whole DRAM is normal WB memory visible to DMA-capable
 * virtio-pci devices, so a simple bump allocator over a static window suffices. */
#define SHARED_REGION_SIZE (64UL * 1024 * 1024)
static uint8_t g_shared_region[SHARED_REGION_SIZE] __attribute__((aligned(64 * 1024)));
static size_t g_shared_region_used;
static spinlock_t g_shared_region_lock = INIT_SPINLOCK_UNLOCKED;

void* memory_get_shared_region(size_t size) {
    spinlock_lock(&g_shared_region_lock);
    size = ALIGN_UP(size, 64);
    if (g_shared_region_used + size > SHARED_REGION_SIZE) {
        spinlock_unlock(&g_shared_region_lock);
        return NULL;
    }
    void* ret = &g_shared_region[g_shared_region_used];
    g_shared_region_used += size;
    spinlock_unlock(&g_shared_region_lock);
    return ret;
}

int memory_free_shared_region(void* addr, size_t size) {
    __UNUSED(addr);
    __UNUSED(size);
    /* Bump allocator: individual regions are recycled at higher layers (virtio drivers reuse
     * their rings); reclaiming the middle of the window is a follow-up like the x86 "free shared
     * region" reclamation that also relies on allocation patterns not racing. */
    return 0;
}

/*
 * ---------------------------------------------------------------------------------------------
 * VMSA tables (4KB granule, TCR.T0SZ = 25 -> 39-bit VA, walk starts at L1; IPS = 0b010 ->
 * 40-bit PA). Identity mapping everywhere, so a descriptor's output address equals the VA bits
 * it translates and a table's physical address equals the pointer to it.
 * ---------------------------------------------------------------------------------------------
 */

#define PT_INDEX_L1(va) (((va) >> 30) & 0x1FF)
#define PT_INDEX_L2(va) (((va) >> 21) & 0x1FF)
#define PT_INDEX_L3(va) (((va) >> 12) & 0x1FF)

#define PT_ADDR_MASK   (((0xFFULL << 32) | 0xFFFFFFF000ULL)) /* 40-bit OA, page-aligned (IPS = 2) */
#define PT_TYPE_MASK   0x3ULL
#define PT_VALID       0x1ULL            /* bit 0: valid at any level */
#define PT_TABLE       0x3ULL            /* bits [1:0] == 0b11: table at L1/L2 */
#define PT_BLOCK       0x1ULL            /* bits [1:0] == 0b01: block leaf at L1/L2. VMSA leaves
                                          * no way to "or this in": a block is exactly a valid
                                          * descriptor that does NOT set bit 1. Writing `| 2`
                                          * onto 0x701 produces 0x703 == table, which is what
                                          * took the first fine-table boot down with a
                                          * translation fault at L3 (the walker read the 512
                                          * "leaf descriptors" out of the block's own RAM). */
#define PT_PAGE        0x3ULL            /* VMSA's Level-3 PAGE encoding is bits[1:0] == 0b11 --
                                          * the same as a table descriptor, which is legal
                                          * because the walk cannot descend below 3. The 0b01
                                          * "block" encoding is architecturally RESERVED at
                                          * level 3 (AArch64.BlockDescSupported() covers levels
                                          * 1 and 2 only; upstream ptw.c calls the 0b01-at-L3
                                          * fault "the Reserved level 3 encoding"), and QEMU's
                                          * restart_atomic_update gate implements exactly that
                                          * rule: an L3 leaf written 0x7c1 took a translation
                                          * fault at level 3 on every fetch of PAL text, while
                                          * the very same 0b01 descriptors at levels 1/2 of the
                                          * same walk translated fine. */

#define PTE_ATTRINDX_SHIFT 2                                   /* bits [4:2], all levels */
#define PTE_ATTR_NORMAL    (0x0ULL << PTE_ATTRINDX_SHIFT)      /* MAIR attr0: Normal WBWA */
#define PTE_ATTR_DEVICE    (0x1ULL << PTE_ATTRINDX_SHIFT)      /* MAIR attr1: Device-nGnRE */
#define PTE_AP_SHIFT       6
#define PTE_AP_EL1ONLY     (0x0ULL << PTE_AP_SHIFT)            /* EL1 rw,      EL0 none */
#define PTE_AP_EL0RW       (0x1ULL << PTE_AP_SHIFT)            /* EL1 rw,      EL0 rw  */
#define PTE_AP_BOTHRO      (0x3ULL << PTE_AP_SHIFT)            /* EL1 ro + X,  EL0 ro  */
#define PTE_SH_INNER       (0x3ULL << 8)                       /* bits [9:8] */
#define PTE_AF             (0x1ULL << 10)                       /* access flag: QEMU faults on 0 */
#define PTE_PXN            (0x1ULL << 53)                       /* unprivileged-XN never; PXN = 0
                                                                 * everywhere: the kernel *must*
                                                                 * fetch LibOS text for syscall
                                                                 * dispatch and PAL text is the
                                                                 * same kind of exec segment */
#define PTE_UXN            (0x1ULL << 54)

/* everything a split of a block into a finer table must carry over: descriptor bits [11:0]
 * (type, AttrIndx [4:2], AP [7:6], SH [9:8], AF [10] -- and for a leaf also the type bits 0b10,
 * which mean "page" at L3 just like "block" at L2) plus the two execute-never bits. The output
 * address is NOT part of this mask: it is taken separately from PT_ADDR_MASK and would be
 * double-counted at every split page otherwise. */
#define PTE_INHERIT (0xFFFULL | PTE_PXN | PTE_UXN)

/* Default protection for plain data pages (kernel data included): writable at EL1 and EL0,
 * never executable at EL0 (W^X for userland). QEMU's xn rule makes the same pages non-executable
 * at EL1 for free. PT_VALID (bit 0) is part of these constants on purpose: unlike the x86 PDE
 * layout there is no implicit "present" encoding -- 0x740 differs from 0x741 by exactly the bit
 * whose absence once cost a boot where every table was valid and every leaf was not. */
#define PTE_DATA (PT_VALID | PTE_AF | PTE_SH_INNER | PTE_ATTR_NORMAL | PTE_AP_EL0RW | PTE_UXN)
/* Executable segments (PAL text, LibOS text, app text): the only AP encoding under which both
 * EL1 and EL0 may fetch (see the file header). Both become read-only, which is exactly right
 * for text once the loaders have finished (pal_rtld/_PalStreamMap populate under W and flip to
 * the final prot via _PalVirtualMemoryProtect). */
#define PTE_EXEC (PT_VALID | PTE_AF | PTE_SH_INNER | PTE_ATTR_NORMAL | PTE_AP_BOTHRO)

/* Bootloader's MAIR: attr0 Normal WBWA, attr1 Device-nGnRE, attr2 0x44 (reserved WC). */

/* Page-table page pool. The kernel image lives below 0x44200000 and TCR.T0SZ = 25 caps the VA at
 * 512GB, so 3 fixed L2 tables (device + 2 DRAM GBs) plus the L1 and a few dozen L3 tables cover
 * boot; runtime L3 promotions are driven by exec-segment mappings (one table per 2MB block,
 * i.e. per straddled 2MB boundary): libsysdb + app + mprotect churn need far fewer than the 512
 * tables (~2MB of .bss) reserved here. The pool is pre-zeroed .bss and bump-allocated, so new
 * tables need no init; identity mapping makes `ptr` its own physical address. */
/* Each 4KB table maps 2MB of fine-grained (4KB-page) mappings, so 2048 tables cover ~4GB of
 * application mappings (512 covered 1GB, and a large mmap() then aborted the whole VM). */
#define PT_POOL_TABLES 2048
static uint64_t g_pt_pool[PT_POOL_TABLES][512] __attribute__((aligned(PAGE_SIZE)));
static size_t g_pt_pool_used;

/* Installed TTBR0_EL1 root (NULL before memory_pagetables_init() switched).
 * Hidden-visibility but not static: the pal_ap_entry trampoline in vm_bootloader.S installs
 * *this* live root on every PSCI-brought-up CPU, and init_multicore() asserts it is
 * non-NULL before issuing any CPU_ON. The bootloader's coarse initial_l1_table maps DRAM
 * as EL1-only 1GB blocks, so an AP running on those tables permission-faults at EL0 the
 * first moment a scheduler-migrated thread touches memory there -- the live root is the
 * one with the EL0-accessible leaves memory_mark_pages_on() installs. */
__attribute__((visibility("hidden"))) uint64_t* g_page_table_root;

/* Boot-time exhaustion (memory_pagetables_init) is fatal; once the live root is installed, running
 * out is just "the guest asked for too much" and must fail the mapping with ENOMEM, not abort the
 * VM. Returns NULL in that case. */
static uint64_t* pt_alloc_table(const char* what) {
    if (g_pt_pool_used >= PT_POOL_TABLES) {
        if (!g_page_table_root) {
            INIT_FAIL("out of page-table pool while creating %s table", what);
        }
        return NULL;
    }
    return &g_pt_pool[g_pt_pool_used++][0];
}

/*
 * Mutating the identity page tables is *shared-state work*: pt_get_leaf(create=true) allocates
 * an L3 table and swaps it into an L2 block descriptor (l3_from_block), and a second CPU
 * splitting the same block concurrently would re-swap in its own replacement table, detaching
 * the first CPU's half-populated one -- mappings vanish silently and the victims fault at EL0
 * (observed on PSCI SMP as a SIGSEGV at a valid stack address the moment LibOS ran on >1 CPU).
 * One global lock is the architecturally correct granularity here: splits and whole-block
 * teardown can touch the same descriptors from any CPU, so finer (per-L2) locking would need
 * per-descriptor state the tables themselves do not keep. Read-only lookups
 * (memory_find_page_table_entry, and l3_from_block's snapshot read inside the lock) stay
 * lock-free deliberately: their caller -- the panic/instruction-dump diagnostic path -- can run
 * at EL1 with IRQs masked *on a CPU that may itself have been preempted while holding this
 * lock*, where taking it would self-deadlock; and word-sized descriptor reads can only show
 * either the old block or the new table, both of which are walkable (the diagnostics path also
 * tolerates a stale answer: worst case it reports "unmapped" about a mapped page).
 */
static spinlock_t g_page_table_lock = INIT_SPINLOCK_UNLOCKED;

/* Flush this CPU's entries and complete the broadcast with the SGI shootdown protocol:
 * QEMU's TCG treats tlbi vmalle1is as a *local* flush (no cross-vCPU shootdown in the
 * emulator -- see the long comment in arch/aarch64/kernel_interrupts.c), so the other CPUs
 * are dropped via send_invalidate_tlb_ipi_and_wait() instead of trusting the DSB. Must be
 * called with g_page_table_lock held: page_table_lock -> request_lock is the one and only
 * lock order in this subsystem (the SGI receiver takes neither). No-ops during boot and on
 * a single-CPU kernel, where this CPU is by definition the only one that can run. */
static void tlb_flush_all(void) {
    /* the PTE/descriptor stores must be observable by every walker before the invalidate
     * (Linux: dsb(ishst) before tlbi); then broadcast (the receivers' IS flush is the same) */
    __asm__ volatile("dsb ishst\n\ttlbi vmalle1is\n\tdsb ish\n\tisb" ::: "memory");
    int ret = send_invalidate_tlb_ipi_and_wait(NULL, 0, /*invalidate_on_this_cpu=*/false);
    if (ret < 0) {
        /* cannot happen: every sender goes through g_page_table_lock, so no protocol run
         * can be in progress on another CPU while we hold it */
        log_warning("tlb_flush_all: TLB shootdown IPI failed (%d); other CPUs may keep "
                    "stale entries", ret);
    }
}

static bool l1_range_built(uint64_t va) {
    uint64_t* l1 = g_page_table_root;
    return l1 && (l1[PT_INDEX_L1(va)] & PT_TYPE_MASK) == PT_TABLE;
}

/* Split the L2 block descriptor at `l2e` into a full L3 table that reproduces its mapping.
 * Returns the L3 table; `l2e` must be a valid block descriptor. */
static uint64_t* l3_from_block(uint64_t* l2e) {
    uint64_t inherit = *l2e & PTE_INHERIT;
    uint64_t base = *l2e & PT_ADDR_MASK;
    uint64_t* l3 = pt_alloc_table("L3 (split)");
    if (!l3) {
        return NULL;
    }
    for (unsigned i = 0; i < 512; i++) {
        l3[i] = PT_PAGE | base + i * PAGE_SIZE + inherit;
    }
    /* The L3 entries must be visible to every CPU's table walker *before* the L2 descriptor
     * that points at them is: the walker is not ordered against our plain stores, so without
     * this DSB another CPU can see the new table slot with still-zero entries and take a
     * translation fault on a page that is (and always was) mapped. */
    __asm__ volatile("dsb ishst" ::: "memory");
    *l2e = ((uint64_t)(uintptr_t)l3 & PT_ADDR_MASK) | PT_TABLE;
    __asm__ volatile("dsb ishst" ::: "memory");
    return l3;
}

/* DRAM window described by memory_pagetables_init(); used to re-create 2MB backing when a range
 * that memory_mark_pages_off() unmapped is mapped again (munmap-then-mmap must work). */
static uint64_t g_dram_base, g_dram_end;

/* Get the leaf descriptor for `va`, splitting blocks on demand. With `create`, DRAM addresses
 * whose 2MB block was taken down by memory_mark_pages_off() are first re-created with the
 * default PTE_DATA protection. Returns NULL when the region has no backing and cannot get one
 * (outside installed DRAM, device world, ...) -- callers report that as a rejected operation
 * rather than faulting. */
static uint64_t* pt_get_leaf(uint64_t va, bool create) {
    if ((l1_range_built(va))) {
        uint64_t* l2t = (uint64_t*)(uintptr_t)(g_page_table_root[PT_INDEX_L1(va)] & PT_ADDR_MASK);
        uint64_t* l2e = &l2t[PT_INDEX_L2(va)];
        if ((*l2e & PT_TYPE_MASK) == PT_TABLE) {
            uint64_t* l3 = (uint64_t*)(uintptr_t)(*l2e & PT_ADDR_MASK);
            return &l3[PT_INDEX_L3(va)];
        }
        if ((*l2e & PT_TYPE_MASK) == PT_BLOCK) {
            if (!create) {
                return l2e;
            }
            uint64_t* l3 = l3_from_block(l2e);
            return l3 ? &l3[PT_INDEX_L3(va)] : NULL;
        }
        if (create && va >= g_dram_base && va < g_dram_end) {
            /* the whole 2MB block was marked off: reinstate it, then split */
            *l2e = (va & ~(0x00200000ULL - 1)) | PTE_DATA;
            __asm__ volatile("dsb ishst" ::: "memory");
            uint64_t* l3 = l3_from_block(l2e);
            return l3 ? &l3[PT_INDEX_L3(va)] : NULL;
        }
        return NULL; /* invalid block/table slot */
    }
    return NULL;
}

int memory_find_page_table_entry(uint64_t addr, uint64_t** out_pte_addr) {
    uint64_t* leaf = pt_get_leaf(ALIGN_DOWN(addr, PAGE_SIZE), /*create=*/false);
    if (!leaf || !(*leaf & PT_VALID)) {
        return -PAL_ERROR_DENIED;
    }
    *out_pte_addr = leaf;
    return 0;
}

int memory_mark_pages_on(uint64_t addr, size_t size, bool write, bool execute, bool usermode) {
    if (!g_page_table_root) {
        /* boot-early call before the fine tables exist: the bootloader's identity map already
         * covers everything; nothing to do (only reachable before memory_pagetables_init()). */
        return 0;
    }

    /* the leaf protection bits: PT_PAGE supplies the level-3 0b11 encoding; the same
     * PTE_DATA/PTE_EXEC constants are also ORed into level-1/2 *blocks* elsewhere, where
     * their PT_VALID-only bits[1:0] = 0b01 is the correct block encoding. */
    uint64_t prot = PT_PAGE | PTE_AF | PTE_SH_INNER | PTE_ATTR_NORMAL;
    if (!execute) {
        prot |= PTE_UXN;
    }
    if (usermode) {
        prot |= write ? PTE_AP_EL0RW : PTE_AP_BOTHRO;
    } else {
        prot |= PTE_AP_EL1ONLY;
    }

    uint64_t start = ALIGN_DOWN(addr, PAGE_SIZE);
    uint64_t end = ALIGN_UP(addr + size, PAGE_SIZE);

    spinlock_lock(&g_page_table_lock);
    for (uint64_t va = start; va < end; va += PAGE_SIZE) {
        uint64_t* leaf = pt_get_leaf(va, /*create=*/true);
        if (!leaf) {
            log_warning("memory_mark_pages_on: no page-table backing for %p (range %p-%p)",
                        (void*)va, (void*)start, (void*)end);
            spinlock_unlock(&g_page_table_lock);
            return -PAL_ERROR_DENIED;
        }
        *leaf = (va & PT_ADDR_MASK) | prot; /* identity: output address == virtual address */
    }
    tlb_flush_all();
    spinlock_unlock(&g_page_table_lock);
    return 0;
}

int memory_mark_pages_off(uint64_t addr, size_t size) {
    if (!g_page_table_root) {
        return 0;
    }

    uint64_t start = ALIGN_DOWN(addr, PAGE_SIZE);
    uint64_t end = ALIGN_UP(addr + size, PAGE_SIZE);

    spinlock_lock(&g_page_table_lock);
    for (uint64_t va = start; va < end;) {
        /* whole-block fast path: take down the 2MB descriptor itself instead of promoting the
         * block just to clear every leaf in it (a large munmap must not burn the table pool).
         * pt_get_leaf(create=false) on a block returns the L2 descriptor itself. */
        if ((va & (0x00200000ULL - 1)) == 0 && va + 0x00200000 <= end) {
            uint64_t* leaf = pt_get_leaf(va, /*create=*/false);
            if (leaf && (*leaf & PT_TYPE_MASK) == PT_BLOCK) {
                *leaf = 0;
                va += 0x00200000;
                continue;
            }
        }
        uint64_t* leaf = pt_get_leaf(va, /*create=*/true);
        if (!leaf) {
            va += PAGE_SIZE;
            continue; /* never mapped: already off */
        }
        *leaf = 0;
        va += PAGE_SIZE;
    }
    tlb_flush_all();
    spinlock_unlock(&g_page_table_lock);
    return 0;
}

int memory_mark_pages_strong_uncacheable(uint64_t addr, size_t size, bool mark) {
    __UNUSED(addr);
    __UNUSED(size);
    __UNUSED(mark);
    /* TODO(Milestone-B): switch MAIR index to Device-nGnRnE per page. No consumer on arm64
     * (this is the x86 hugepage/graphics path). */
    return 0;
}

int memory_mark_pages_write_combined(uint64_t addr, size_t size, bool mark) {
    __UNUSED(addr);
    __UNUSED(size);
    __UNUSED(mark);
    /* TODO(Milestone-B): switch MAIR index to WC (attr2 = 0x44 is installed) per page. No
     * consumer on arm64 until graphics forwarding is ported. */
    return 0;
}

/* Guest-visible RAM is [g_dram_base, g_dram_end); everything below it is flash/MMIO windows of the
 * `virt` machine. Handing such a range to an application (e.g. LibOS mapping a non-PIE ET_EXEC linked
 * at 0x400000) used to "succeed" and then memset megabytes of emulated flash/device space at MMIO
 * speed, i.e. look like a hang. Refuse it up front; the boot-time DRAM window is unknown (== 0)
 * only before memory_pagetables_init(). */
static bool range_outside_dram(uint64_t addr, size_t size) {
    return g_dram_end && (addr < g_dram_base || addr + size < addr || addr + size > g_dram_end);
}

int memory_alloc(void* addr, size_t size, bool read, bool write, bool execute) {
    if (!read && !write && !execute) {
        return memory_mark_pages_off((uint64_t)addr, size);
    }
    if (range_outside_dram((uint64_t)addr, size)) {
        return -PAL_ERROR_INVAL;
    }

    /* The x86 version maps with the final protection and memsets directly, relying on CR0.WP ==
     * 0 to let ring 0 write into ring-3 read-only pages. VMSA's simple AP has no such kernel
     * bypass for AP = 0b11 (EL1 read-only is EL1 read-only), so map writable first, zero, and
     * demote to the requested read-only protection afterwards. Single-core, and no EL0 thread
     * can observe a page between these two calls during a given thread's own allocation. */
    int ret = memory_mark_pages_on((uint64_t)addr, size, /*write=*/true, execute,
                                   /*usermode=*/true);
    if (ret < 0) {
        return ret;
    }

    memset(addr, 0, size);

    if (!write) {
        ret = memory_mark_pages_on((uint64_t)addr, size, /*write=*/false, execute,
                                   /*usermode=*/true);
    }
    return ret;
}

int memory_protect(void* addr, size_t size, bool read, bool write, bool execute) {
    if (!read && !write && !execute) {
        return memory_mark_pages_off((uint64_t)addr, size);
    }
    if (range_outside_dram((uint64_t)addr, size)) {
        return -PAL_ERROR_INVAL;
    }
    return memory_mark_pages_on((uint64_t)addr, size, write, execute, /*usermode=*/true);
}

int memory_free(void* addr, size_t size) {
    return memory_mark_pages_off((uint64_t)addr, size);
}

int memory_tighten_permissions(void) {
    /* TODO(Milestone-B): PXN the data segments so EL1 cannot fetch them either; QEMU's xn rule
     * already forbids EL1 fetch from the EL0-rw data pages for free, so this is hardening for
     * real hardware, not a correctness requirement under QEMU. */
    return 0;
}

void memory_pat_init_for_wc(void) {
    /* No PAT register on aarch64; WC is MAIR attr2 (installed by the bootloader). */
}

/*
 * ---------------------------------------------------------------------------------------------
 * TTBR1: a supervisor-only direct map of DRAM ("frame alias"), the counterpart of the x86
 * window at 0xffff800000000000 that the x86 code installs in every process root. On AArch64 the
 * upper VA range has its own translation base, so every TTBR0 root sees the same window without
 * copying any entry. TCR_EL1.T1SZ = 25 gives the upper 512 GB, [0xffffff8000000000, 2^64): the
 * alias of physical address `pa` is FRAME_ALIAS_BASE + pa, so the L1 index is pa >> 30.
 * ---------------------------------------------------------------------------------------------
 */
#define FRAME_ALIAS_BASE 0xffffff8000000000UL
#define PAGE_2M_SIZE     0x00200000UL

/* TCR_EL1 fields of the TTBR1 half: T1SZ = 25, IRGN1/ORGN1 = Normal WBWA, SH1 = inner
 * shareable, TG1 = 4 KB (TG1's encoding differs from TG0's: 0b10 is 4 KB), EPD1 = 0 (walks
 * enabled), A1 = 0 (the ASID comes from TTBR0; no ASIDs are used). pal_ap_entry in
 * vm_bootloader.S writes the same value on the secondary CPUs. */
#define TCR_T1_FIELDS ((25UL << 16) | (1UL << 24) | (1UL << 26) | (3UL << 28) | (2UL << 30))
#define TCR_T1_MASK   ((0x3fUL << 16) | (1UL << 22) | (1UL << 23) | (3UL << 24) | (3UL << 26) \
                       | (3UL << 28) | (3UL << 30))

static uint64_t g_alias_l1[512] __attribute__((aligned(PAGE_SIZE)));
/* physical address of g_alias_l1, installed in TTBR1_EL1 by every CPU (pal_ap_entry reads it) */
__attribute__((visibility("hidden"))) uint64_t g_ttbr1_root;
/* end of the 2 MB-aligned DRAM range that the alias maps */
static uint64_t g_alias_end;
/* the physical address of the boot TTBR0 root; the x86 name is kept because the shared
 * multiprocess code (pal_process.c) compares roots against it */
uint64_t g_pml4_table_base;
/* the boot root as an address space (PAL helper threads run in it; set up with the tables) */
static struct memory_address_space g_boot_address_space;

static void clean_dcache_line(const void* addr) {
    __asm__ volatile("dc civac, %0\n\tdsb ish" :: "r"(addr) : "memory");
}

static void frame_alias_tables_init(uint64_t mem_base, uint64_t mem_end) {
    uint64_t end = ALIGN_DOWN(mem_end, PAGE_2M_SIZE);
    for (uint64_t gb = mem_base >> 30; gb <= ((end - 1) >> 30); gb++) {
        uint64_t* l2 = pt_alloc_table("L2 (frame alias)");
        for (unsigned i = 0; i < 512; i++) {
            uint64_t pa = (gb << 30) + (uint64_t)i * PAGE_2M_SIZE;
            if (pa < mem_base || pa + PAGE_2M_SIZE > end)
                continue;
            l2[i] = pa | PT_BLOCK | PTE_AF | PTE_SH_INNER | PTE_ATTR_NORMAL | PTE_AP_EL1ONLY
                    | PTE_PXN | PTE_UXN;
        }
        g_alias_l1[gb] = ((uint64_t)(uintptr_t)l2 & PT_ADDR_MASK) | PT_TABLE;
    }
    __asm__ volatile("dsb ishst" ::: "memory");
    g_alias_end = end;
    g_ttbr1_root = (uint64_t)(uintptr_t)g_alias_l1;
}

/*
 * Build and install the fine-grained identity tables. Mirrors x86's memory_pagetables_init():
 * coarse (bootloader) tables -> real ones -> swap TTBR and invalidate. Called from
 * pal_start_c() before any driver or allocator can fault, with interrupts still masked.
 */
int memory_pagetables_init(void* memory_address_end, bool current_page_tables_cover_1gb) {
    __UNUSED(current_page_tables_cover_1gb);

    extern const char __text_start[];
    extern const char __text_end[];
    const uint64_t text_start = (uint64_t)__text_start; /* already page-aligned (pal.lds) */
    const uint64_t text_end = ALIGN_UP((uint64_t)__text_end, PAGE_SIZE);

    const uint64_t mem_base = g_dtb_mem_start;
    const uint64_t mem_end = ALIGN_DOWN((uint64_t)(uintptr_t)memory_address_end, PAGE_SIZE);
    if (mem_end <= mem_base) {
        return -PAL_ERROR_INVAL;
    }
    g_dram_base = mem_base;
    g_dram_end = mem_end;

    /* --- new L1 (walk root, T0SZ = 25: index = VA[38:30]) --- */
    uint64_t* l1 = pt_alloc_table("L1");

    /* [0, 1GB): same device/normal split as the bootloader's initial_l2_low:
     * [0x00000000, 0x08000000) flash alias and [0x0c000000, 0x10000000) are "normal" blocks with
     * nothing attached; GIC/PL011/fw_cfg/virtio-mmio and the PCIe low window + ECAM are device
     * memory (MAIR attr1). All EL1-only (AP = 0b00): userland must never touch MMIO. */
    uint64_t* l2_dev = pt_alloc_table("L2 (device)");
    for (unsigned i = 0; i < 512; i++) {
        uint64_t va = (uint64_t)i * 0x00200000;
        bool normal = (i < 64) || (i >= 96 && i < 128);
        l2_dev[i] = PT_VALID | va | PTE_AF | PTE_SH_INNER
                  | (normal ? PTE_ATTR_NORMAL : PTE_ATTR_DEVICE) | PTE_AP_EL1ONLY;
    }
    l1[0] = ((uint64_t)(uintptr_t)l2_dev & PT_ADDR_MASK) | PT_TABLE;

    /* [1GB, ...): one L2 per installed DRAM GB. Default mapping for every 2MB block is PTE_DATA
     * (EL1 rw + EL0 rw + UXN, normal WB); pages inside the kernel's rx segment become PTE_EXEC
     * via an L3 split. Free DRAM is left EL0-rw because the x86 boot state leaves ring-3 with
     * access to all identity-mapped memory (libsysdb reads kernel data directly); the slab
     * bookkeeping allocator is what keeps user allocations away from kernel ones. */
    for (uint64_t gb = mem_base >> 30; gb <= ((mem_end - 1) >> 30); gb++) {
        uint64_t* l2 = pt_alloc_table("L2 (DRAM)");
        l1[gb] = ((uint64_t)(uintptr_t)l2 & PT_ADDR_MASK) | PT_TABLE;

        for (unsigned i = 0; i < 512; i++) {
            uint64_t va = (gb << 30) + (uint64_t)i * 0x00200000;
            if (va < text_end && va + 0x00200000 > text_start) {
                /* block straddles the kernel rx segment: per-page exec split */
                uint64_t* l3 = pt_alloc_table("L3 (kernel text)");
                for (unsigned j = 0; j < 512; j++) {
                    uint64_t pv = va + (uint64_t)j * PAGE_SIZE;
                    bool exec = pv >= text_start && pv < text_end;
                    l3[j] = (pv & PT_ADDR_MASK) | PT_PAGE | (exec ? PTE_EXEC : PTE_DATA);
                }
                l2[i] = ((uint64_t)(uintptr_t)l3 & PT_ADDR_MASK) | PT_TABLE;
            } else {
                l2[i] = (va & PT_ADDR_MASK) | PTE_DATA;
            }
        }
    }

    /* The supervisor-only direct map of DRAM in TTBR1 (the multiprocess support reaches physical
     * frames and page tables through it, whatever the current TTBR0 root maps): built for every
     * boot so that the secondary CPUs can install it in pal_ap_entry like the TTBR0 root; nothing
     * uses it before the frame pool is set up. */
    frame_alias_tables_init(mem_base, mem_end);

    /* Install. The switch is transparent: the tables describe the same identity mapping, the
     * current stack is PTE_DATA (.bss below the rx segment, EL1 rw with PAN cleared), and the
     * next fetch is PAL text -- PTE_EXEC, EL1 r+x -- so nothing between `msr` and `isb` depends
     * on the old table. Interrupts are still masked (unmasked only after gic_init), and the
     * vector table itself will be fetched from PTE_EXEC pages once installed. */
    g_page_table_root = l1;
    g_pml4_table_base = (uint64_t)(uintptr_t)l1;
    g_boot_address_space.root = l1;
    g_boot_address_space.root_phys = (uint64_t)(uintptr_t)l1;
    uint64_t tcr;
    __asm__ volatile("mrs %0, tcr_el1" : "=r"(tcr));
    tcr = (tcr & ~TCR_T1_MASK) | TCR_T1_FIELDS;
    __asm__ volatile(
        "msr ttbr1_el1, %1\n\t"
        "msr tcr_el1, %2\n\t"
        "msr ttbr0_el1, %0\n\t"
        "isb\n\t"
        "tlbi vmalle1\n\t"
        "dsb ish\n\t"
        "isb\n\t"
        ::"r"(l1), "r"(g_ttbr1_root), "r"(tcr)
        : "memory");
    /* pal_ap_entry reads these two with the MMU (and so the data cache) still off */
    clean_dcache_line(&g_page_table_root);
    clean_dcache_line(&g_ttbr1_root);

    log_debug("VMSA identity tables installed: TTBR0_EL1 %p, DRAM [%p, %p), kernel text [%p, %p)"
              " mapped AP=0b11", l1, (void*)mem_base, (void*)mem_end, (void*)text_start,
              (void*)text_end);
    return 0;
}

/*
 * Memory map init: `e820_entries` here is the *synthetic* one-entry table pal_main.c builds from
 * the DTB /memory node (kept e820-shaped so this function and memory_preload_ranges share the
 * x86 prototypes). Returns the usable RAM window for the PAL allocator: everything above the
 * 32MB reserved area at the start of DRAM (where the kernel + its tables live).
 */
int memory_init(e820_table_entry* e820_entries, size_t e820_entries_size,
                void** out_memory_address_start, void** out_memory_address_end) {
    uint64_t mem_base = g_dtb_mem_start;
    uint64_t mem_size = g_dtb_mem_size;

    if (e820_entries && e820_entries_size >= sizeof(e820_table_entry)) {
        /* trust the synthetic entry; it is derived from the same DTB but pal_main owns that */
        mem_base = e820_entries[0].address;
        mem_size = e820_entries[0].size;
    }

    if (mem_size <= ARM_KERNEL_RESERVE_SIZE + 16UL * 1024 * 1024)
        return -PAL_ERROR_INVAL; /* absurdly small VM */

    uint64_t start = mem_base + ARM_KERNEL_RESERVE_SIZE;
    uint64_t end   = mem_base + mem_size;

    /* The whole kernel image (ro + rw + 64MB .bss shared pool + rx, ending at __text_end) must
     * live *below* the allocator floor, exactly like x86 keeps the PAL binary below
     * memory_address_start. 32MB was the pre-B1 image guess; clamp to the linker-provided image
     * end so the floor adapts as the kernel grows (the slab bookkeeping allocator refuses to
     * hand out anything overlapping an initial range, and the identity boot map makes a static
     * floor cheaper than per-page reservations). */
    extern const char __text_end[];
    uint64_t kernel_image_end = ALIGN_UP((uint64_t)__text_end, PAGE_SIZE);
    if (start < kernel_image_end) {
        start = kernel_image_end;
    }

    if (start >= end)
        return -PAL_ERROR_DENIED; /* RAM too small to host the kernel + a usable heap */

    /* keep the end page-aligned and below the first device region if DRAM were to reach it */
    end = ALIGN_DOWN(end, PAGE_SIZE);

    *out_memory_address_start = (void*)start;
    *out_memory_address_end   = (void*)end;
    return 0;
}

int memory_preload_ranges(e820_table_entry* e820_entries, size_t e820_entries_size,
                          int (*callback)(uintptr_t addr, size_t size, bool is_user_reserved,
                                          const char* comment)) {
    __UNUSED(e820_entries);
    __UNUSED(e820_entries_size);
    __UNUSED(callback);
    /* Nothing to report. Initial ranges are *obstacles* for the slab bookkeeping allocator
     * (pal/src/pal_memory.c initial_mem_bkeep carves new memory below memory_address_end and
     * skips around them), not a description of usable RAM: registering the whole identity-mapped
     * DRAM as one range - as an earlier B0 revision did under the comment "the slab allocator
     * bookkeeps freeness on top of it" - made every candidate overlap it, got pushed to its
     * start, fell below memory_address_start and failed the first system_malloc() of slab init
     * at *every* retry size. The x86 layout's equivalent protection comes from two places we do
     * keep: memory_init() clamps the floor above __text_end, and pal_main.c adds the defensive
     * "pal_binary" obstacle. Device/MMIO ranges need no obstacle either: the DTB does not
     * describe them as E820 memory, so the allocator never sees them. */
    return 0;
}

/*
 * =============================================================================================
 * Multiprocess support (`sys.experimental__multiprocess`): the AArch64 implementation of the
 * frame pool and of the per-process address spaces declared in kernel_memory.h. The design is
 * the x86_64 one (vm-common/kernel_memory.c, Documentation/devel/vm-multiproc/FINDINGS.md):
 *
 *   - application frames come from a pool, a reserved interval of DRAM that the boot root maps
 *     EL1-only; every frame and every page table is reached through the TTBR1 alias above, never
 *     through its identity address (which a process root may map to something else);
 *   - a process root is a private L1 table. The first process root is a lazy copy of the boot
 *     root: it shares the boot root's L2/L3 tables (the kernel, PAL and device entries) until a
 *     mapping in it needs a private table, which is then copied (a 2 MB block is split). A root
 *     cloned from another process root (fork, spawn) gets private copies of every table of its
 *     source, so that it does not depend on its parent's lifetime;
 *   - a leaf that maps a pool frame holds a reference on it; a forked child shares its parent's
 *     frames until it replaces them, and destroying a root drops all its references;
 *   - allocations are demand-zero: a lazy invalid leaf gets a zeroed frame at its first fault;
 *   - the scheduler installs the next thread's root in TTBR0 (kernel_context.S) with a full local
 *     TLB invalidation (no ASIDs); page-table changes are made visible to the other vCPUs with the
 *     acknowledged shootdown of kernel_interrupts.c.
 * Nothing here runs unless the manifest key is set: without it the frame pool does not exist and
 * every thread stays in the boot root.
 * =============================================================================================
 */

/* Software marker of an invalid level-3 descriptor (bit 0 clear, so the hardware ignores every
 * other bit): a demand-zero page whose access rights are kept in the entry's AP and UXN bits. A
 * frame kept in an invalid entry (between an unmap and the TLB shootdown, or under PROT_NONE)
 * keeps its output address in bits [39:12] as in a valid one. */
#define PTE_LAZY       (1ULL << 58)
#define PTE_AP_MASK    (0x3ULL << PTE_AP_SHIFT)
/* the bits of a user leaf that hold its access rights */
#define PTE_RIGHTS     (PTE_AP_MASK | PTE_UXN)
#define PTE_USER_PAGE  (PT_PAGE | PTE_AF | PTE_SH_INNER | PTE_ATTR_NORMAL)

/* fault "error code" bits, the x86 page-fault error code bits that the shared interface uses */
#define FAULT_PRESENT 0x1UL
#define FAULT_WRITE   0x2UL
#define FAULT_FETCH   0x10UL

static uintptr_t g_frame_pool_start;
static size_t g_frame_pool_size;
static size_t g_frame_pool_next;
static uint8_t* g_frame_pool_bitmap;
static uintptr_t* g_frame_owner_next;
static uint8_t* g_frame_owner_kind;
static uint16_t* g_frame_refcount;
static spinlock_t g_frame_pool_lock = INIT_SPINLOCK_UNLOCKED;
static bool g_frame_alias_active;
/* CTR_EL0.IDC / DIC: whether the data cache must be cleaned and the instruction cache invalidated
 * before a page that was written becomes executable (read at pool initialization) */
static bool g_need_dcache_clean = true;
static bool g_need_icache_invalidate = true;

/* The pool is also used by the fault handler (demand-zero pages), which runs with IRQs masked:
 * restore the caller's DAIF instead of unmasking on unlock (x86: pushfq/cli). */
static inline uint64_t frame_pool_lock(void) {
    uint64_t daif;
    __asm__ volatile("mrs %0, daif\n\tmsr daifset, #2" : "=r"(daif) : : "memory");
    spinlock_lock(&g_frame_pool_lock);
    return daif;
}

static inline void frame_pool_unlock(uint64_t daif) {
    spinlock_unlock(&g_frame_pool_lock);
    __asm__ volatile("msr daif, %0" : : "r"(daif) : "memory");
}

void* memory_frame_pointer(uintptr_t frame) {
    if (g_frame_alias_active && frame >= g_dram_base && frame < g_alias_end)
        return (void*)(FRAME_ALIAS_BASE + frame);
    return (void*)frame;
}

bool memory_frame_is_alias_pointer(const void* pointer) {
    uintptr_t addr = (uintptr_t)pointer;
    return g_frame_alias_active && addr >= FRAME_ALIAS_BASE + g_dram_base &&
           addr < FRAME_ALIAS_BASE + g_alias_end;
}

static uintptr_t memory_frame_from_pointer(const void* pointer) {
    if (memory_frame_is_alias_pointer(pointer))
        return (uintptr_t)pointer - FRAME_ALIAS_BASE;
    return (uintptr_t)pointer;
}

static uint64_t current_ttbr0(void) {
    uint64_t root;
    __asm__ volatile("mrs %0, ttbr0_el1" : "=r"(root));
    return root & PT_ADDR_MASK;
}

/* Drops this CPU's translations of one page (all ASIDs; every entry is global anyway). */
static void tlb_flush_page_local(uintptr_t va) {
    __asm__ volatile("dsb ishst\n\ttlbi vaae1, %0\n\tdsb nsh\n\tisb"
                     : : "r"((va >> 12) & ((1UL << 44) - 1)) : "memory");
}

/* Makes the instructions in `frame` (written through a data mapping) visible to instruction
 * fetches through any mapping: clean the data cache to the point of unification unless
 * CTR_EL0.IDC says it is not needed, then invalidate the instruction caches of every CPU unless
 * CTR_EL0.DIC says so. */
static void sync_icache_frame(uintptr_t frame) {
    if (g_need_dcache_clean) {
        uint64_t ctr;
        __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
        size_t line = 4UL << ((ctr >> 16) & 0xf);
        char* p = memory_frame_pointer(frame);
        for (size_t off = 0; off < PAGE_SIZE; off += line)
            __asm__ volatile("dc cvau, %0" : : "r"(p + off) : "memory");
        __asm__ volatile("dsb ish" ::: "memory");
    }
    if (g_need_icache_invalidate)
        __asm__ volatile("ic ialluis\n\tdsb ish\n\tisb" ::: "memory");
}

/* Sets the boot root's level-2 entries of [addr, addr + size) (2 MB aligned) to `attrs` blocks
 * (identity output addresses); used to make the pool EL1-only without splitting its blocks into
 * a thousand level-3 tables of the static pool. */
static int boot_root_set_blocks(uint64_t addr, size_t size, uint64_t attrs) {
    if (!IS_ALIGNED(addr, PAGE_2M_SIZE) || !IS_ALIGNED(size, PAGE_2M_SIZE))
        return -PAL_ERROR_INVAL;
    spinlock_lock(&g_page_table_lock);
    for (uint64_t va = addr; va < addr + size; va += PAGE_2M_SIZE) {
        if (!l1_range_built(va)) {
            spinlock_unlock(&g_page_table_lock);
            return -PAL_ERROR_INVAL;
        }
        uint64_t* l2t = (uint64_t*)(uintptr_t)(g_page_table_root[PT_INDEX_L1(va)] & PT_ADDR_MASK);
        /* a level-3 table that was there stays allocated (the static pool is never freed) */
        l2t[PT_INDEX_L2(va)] = va | attrs;
    }
    tlb_flush_all();
    spinlock_unlock(&g_page_table_lock);
    return 0;
}

/* Out of frames (multiprocess support): a PAL copy into application memory (a process-stream
 * read, a file read) that meets a demand-zero page when the pool is empty cannot fail the access,
 * and ending the process there would leave the PAL's locks held. Such a page gets this one
 * reserved frame instead, so the copy completes into memory nobody reads, and the process is
 * marked: it is ended at its next exception or tick from EL0, where it holds no PAL lock
 * (memory_address_space_current_doomed()). The OOM killer of Linux sends SIGKILL instead. */
static uintptr_t g_oom_sink_frame;
#define OOM_DOOMED_MAX 32
static uintptr_t g_oom_doomed_roots[OOM_DOOMED_MAX];

int memory_frame_pool_init(uintptr_t addr, size_t size) {
    if (g_frame_pool_bitmap || !size || !IS_ALIGNED(addr, PAGE_2M_SIZE) ||
            !IS_ALIGNED(size, PAGE_2M_SIZE) || addr < g_dram_base || addr + size < addr ||
            addr + size > g_alias_end ||
            addr + size > (uintptr_t)g_pal_public_state.memory_address_end)
        return -PAL_ERROR_INVAL;

    uint64_t ctr;
    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
    g_need_dcache_clean = !(ctr & (1UL << 28));      /* IDC */
    g_need_icache_invalidate = !(ctr & (1UL << 29)); /* DIC */

    size_t frame_count = size / PAGE_SIZE;
    size_t bitmap_size = UDIV_ROUND_UP(frame_count, 8);
    uint8_t* bitmap = memory_get_shared_region(bitmap_size);
    size_t owner_next_size = frame_count * sizeof(*g_frame_owner_next);
    size_t owner_kind_size = frame_count * sizeof(*g_frame_owner_kind);
    size_t refcount_size = frame_count * sizeof(*g_frame_refcount);
    uintptr_t* owner_next = memory_get_shared_region(owner_next_size);
    uint8_t* owner_kind = memory_get_shared_region(owner_kind_size);
    uint16_t* refcount = memory_get_shared_region(refcount_size);
    if (!bitmap || !owner_next || !owner_kind || !refcount)
        return -PAL_ERROR_NOMEM;
    memset(bitmap, 0, bitmap_size);
    memset(owner_next, 0, owner_next_size);
    memset(owner_kind, 0, owner_kind_size);
    memset(refcount, 0, refcount_size);

    /* the pool's identity addresses: EL1-only, never executable (frames are used through the
     * alias; a process root maps them at user addresses) */
    int ret = boot_root_set_blocks(addr, size, PT_BLOCK | PTE_AF | PTE_SH_INNER | PTE_ATTR_NORMAL
                                               | PTE_AP_EL1ONLY | PTE_PXN | PTE_UXN);
    if (ret < 0)
        return ret;

    g_frame_owner_next = owner_next;
    g_frame_owner_kind = owner_kind;
    g_frame_refcount = refcount;
    g_frame_pool_start = addr;
    g_frame_pool_size = size;
    g_frame_alias_active = true;
    g_boot_address_space.root = memory_frame_pointer(g_pml4_table_base);
    g_boot_address_space.root_phys = g_pml4_table_base;
    /* the boot root is changed in place: it has no lazy-copy baseline */
    g_boot_address_space.base_root = NULL;
    __atomic_store_n(&g_frame_pool_bitmap, bitmap, __ATOMIC_RELEASE);
    __atomic_store_n(&g_sched_switch_roots, true, __ATOMIC_RELEASE);
    (void)memory_frame_alloc(&g_oom_sink_frame); /* see memory_address_space_handle_oom_fault() */
    log_always("Private frame pool: [%p, %p), %zu frames; TTBR1 frame alias at %p; CTR_EL0 0x%lx"
               " (IDC %d, DIC %d)", (void*)addr, (void*)(addr + size), frame_count,
               (void*)FRAME_ALIAS_BASE, ctr, !g_need_dcache_clean, !g_need_icache_invalidate);
    return 0;
}

int memory_frame_alloc(uintptr_t* out_frame) {
    if (!out_frame || !g_frame_pool_bitmap)
        return -PAL_ERROR_INVAL;

    uint64_t flags = frame_pool_lock();
    size_t frame_count = g_frame_pool_size / PAGE_SIZE;
    for (size_t scanned = 0; scanned < frame_count; scanned++) {
        size_t i = (g_frame_pool_next + scanned) % frame_count;
        uint8_t mask = 1U << (i % 8);
        if (g_frame_pool_bitmap[i / 8] & mask)
            continue;

        g_frame_pool_bitmap[i / 8] |= mask;
        g_frame_pool_next = (i + 1) % frame_count;
        uintptr_t frame = g_frame_pool_start + i * PAGE_SIZE;
        g_frame_owner_next[i] = 0;
        g_frame_owner_kind[i] = 0;
        g_frame_refcount[i] = 1;
        frame_pool_unlock(flags);
        memset(memory_frame_pointer(frame), 0, PAGE_SIZE);
        *out_frame = frame;
        return 0;
    }
    frame_pool_unlock(flags);
    return -PAL_ERROR_NOMEM;
}

int memory_frame_free(uintptr_t frame) {
    if (!g_frame_pool_bitmap || frame < g_frame_pool_start ||
            frame >= g_frame_pool_start + g_frame_pool_size || !IS_ALIGNED(frame, PAGE_SIZE))
        return -PAL_ERROR_INVAL;

    size_t i = (frame - g_frame_pool_start) / PAGE_SIZE;
    uint8_t mask = 1U << (i % 8);
    uint64_t flags = frame_pool_lock();
    if (!(g_frame_pool_bitmap[i / 8] & mask)) {
        frame_pool_unlock(flags);
        return -PAL_ERROR_INVAL;
    }
    g_frame_pool_bitmap[i / 8] &= ~mask;
    g_frame_owner_next[i] = 0;
    g_frame_owner_kind[i] = 0;
    g_frame_refcount[i] = 0;
    if (i < g_frame_pool_next)
        g_frame_pool_next = i;
    frame_pool_unlock(flags);
    return 0;
}

int memory_frame_alloc_contiguous(size_t size, void** out_alias) {
    if (!g_frame_pool_bitmap || !size || !out_alias || size > SIZE_MAX - PAGE_SIZE + 1)
        return -PAL_ERROR_INVAL;

    size_t pages = UDIV_ROUND_UP(size, PAGE_SIZE);
    size_t frame_count = g_frame_pool_size / PAGE_SIZE;
    if (!pages || pages > frame_count)
        return -PAL_ERROR_NOMEM;

    uint64_t flags = frame_pool_lock();
    size_t first = g_frame_pool_next;
    for (size_t scanned = 0; scanned < frame_count; scanned++) {
        size_t start = (first + scanned) % frame_count;
        if (start + pages > frame_count)
            continue;

        size_t j;
        for (j = 0; j < pages; j++) {
            size_t i = start + j;
            if (g_frame_pool_bitmap[i / 8] & (1U << (i % 8)))
                break;
        }
        if (j != pages) {
            scanned += j;
            continue;
        }

        uintptr_t frame = g_frame_pool_start + start * PAGE_SIZE;
        for (j = 0; j < pages; j++) {
            size_t i = start + j;
            g_frame_pool_bitmap[i / 8] |= 1U << (i % 8);
            g_frame_owner_next[i] = 0;
            g_frame_owner_kind[i] = 0;
            g_frame_refcount[i] = 1;
        }
        g_frame_pool_next = (start + pages) % frame_count;
        frame_pool_unlock(flags);
        memset(memory_frame_pointer(frame), 0, pages * PAGE_SIZE);
        *out_alias = memory_frame_pointer(frame);
        return 0;
    }
    frame_pool_unlock(flags);
    return -PAL_ERROR_NOMEM;
}

int memory_frame_free_contiguous(void* alias, size_t size) {
    if (!g_frame_pool_bitmap || !memory_frame_is_alias_pointer(alias) || !size ||
            size > SIZE_MAX - PAGE_SIZE + 1 || !IS_ALIGNED_PTR(alias, PAGE_SIZE))
        return -PAL_ERROR_INVAL;

    uintptr_t frame = memory_frame_from_pointer(alias);
    size_t pages = UDIV_ROUND_UP(size, PAGE_SIZE);
    if (frame < g_frame_pool_start || pages > g_frame_pool_size / PAGE_SIZE ||
            frame - g_frame_pool_start > g_frame_pool_size - pages * PAGE_SIZE)
        return -PAL_ERROR_INVAL;

    size_t start = (frame - g_frame_pool_start) / PAGE_SIZE;
    uint64_t flags = frame_pool_lock();
    for (size_t j = 0; j < pages; j++) {
        size_t i = start + j;
        if (!(g_frame_pool_bitmap[i / 8] & (1U << (i % 8)))) {
            frame_pool_unlock(flags);
            return -PAL_ERROR_INVAL;
        }
    }
    for (size_t j = 0; j < pages; j++) {
        size_t i = start + j;
        g_frame_pool_bitmap[i / 8] &= ~(1U << (i % 8));
        g_frame_owner_next[i] = 0;
        g_frame_owner_kind[i] = 0;
        g_frame_refcount[i] = 0;
    }
    if (start < g_frame_pool_next)
        g_frame_pool_next = start;
    frame_pool_unlock(flags);
    return 0;
}

static bool is_frame_pool_frame(uintptr_t frame) {
    return g_frame_pool_bitmap && frame >= g_frame_pool_start &&
           frame < g_frame_pool_start + g_frame_pool_size && IS_ALIGNED(frame, PAGE_SIZE);
}

/* Takes another leaf reference on an allocated pool frame; other frames are ignored. */
static void memory_frame_get(uintptr_t frame) {
    if (!is_frame_pool_frame(frame))
        return;
    size_t i = (frame - g_frame_pool_start) / PAGE_SIZE;
    uint64_t flags = frame_pool_lock();
    if (!(g_frame_pool_bitmap[i / 8] & (1U << (i % 8))) || g_frame_refcount[i] == UINT16_MAX) {
        frame_pool_unlock(flags);
        log_error("Invalid reference to frame %p", (void*)frame);
        BUG();
    }
    g_frame_refcount[i]++;
    frame_pool_unlock(flags);
}

/* Drops a leaf reference; the last one returns the frame to the pool. The caller must have
 * flushed stale translations of the leaf it removed. */
static void memory_frame_put(uintptr_t frame) {
    if (!is_frame_pool_frame(frame))
        return;
    size_t i = (frame - g_frame_pool_start) / PAGE_SIZE;
    uint64_t flags = frame_pool_lock();
    if (!(g_frame_pool_bitmap[i / 8] & (1U << (i % 8))) || !g_frame_refcount[i]) {
        frame_pool_unlock(flags);
        log_error("Unbalanced release of frame %p", (void*)frame);
        BUG();
    }
    bool last = --g_frame_refcount[i] == 0;
    if (last)
        g_frame_refcount[i] = 1; /* memory_frame_free() below releases it */
    frame_pool_unlock(flags);
    if (last && memory_frame_free(frame) < 0)
        BUG();
}

void memory_frame_ref(uintptr_t frame) {
    memory_frame_get(frame);
}

void memory_frame_unref(uintptr_t frame) {
    memory_frame_put(frame);
}

size_t memory_frame_pool_capacity(void) {
    return g_frame_pool_bitmap ? g_frame_pool_size / PAGE_SIZE : 0;
}

bool memory_frame_pool_active(void) {
    return !!g_frame_pool_bitmap;
}

size_t memory_frame_allocated_count(void) {
    if (!g_frame_pool_bitmap)
        return 0;
    size_t count = 0;
    size_t frame_count = g_frame_pool_size / PAGE_SIZE;
    uint64_t flags = frame_pool_lock();
    for (size_t i = 0; i < frame_count; i++)
        count += !!(g_frame_pool_bitmap[i / 8] & (1U << (i % 8)));
    frame_pool_unlock(flags);
    return count;
}

struct memory_address_space* memory_address_space_boot(void) {
    return &g_boot_address_space;
}

/* Changes of a range of leaves become visible to every vCPU: the acknowledged shootdown with
 * more than one vCPU, a local invalidation otherwise (x86: invlpg per page before this call). */
int memory_address_space_flush_range(uintptr_t addr, size_t size) {
    if (g_num_cpus <= 1) {
        __asm__ volatile("dsb ishst\n\ttlbi vmalle1\n\tdsb nsh\n\tisb" ::: "memory");
        return 0;
    }
    return send_invalidate_tlb_ipi_and_wait((void*)ALIGN_DOWN(addr, PAGE_SIZE),
                                           ALIGN_UP(size, PAGE_SIZE),
                                           /*invalidate_on_this_cpu=*/true);
}

static int address_space_track_frame(struct memory_address_space* space, uintptr_t frame,
                                     bool is_page_table) {
    if (!is_frame_pool_frame(frame))
        return -PAL_ERROR_INVAL;
    size_t i = (frame - g_frame_pool_start) / PAGE_SIZE;
    g_frame_owner_next[i] = space->owned_frame_head;
    g_frame_owner_kind[i] = is_page_table;
    space->owned_frame_head = frame;
    return 0;
}

static int address_space_alloc_table_page(struct memory_address_space* space,
                                          uint64_t** out_page) {
    uintptr_t frame;
    int ret = memory_frame_alloc(&frame);
    if (ret < 0)
        return ret;
    ret = address_space_track_frame(space, frame, /*is_page_table=*/true);
    if (ret < 0) {
        memory_frame_free(frame);
        return ret;
    }
    *out_page = memory_frame_pointer(frame);
    return 0;
}

static uint64_t* table_pointer(uint64_t entry) {
    return memory_frame_pointer(entry & PT_ADDR_MASK);
}

static bool is_table(uint64_t entry) {
    return (entry & PT_TYPE_MASK) == PT_TABLE;
}

static bool is_block(uint64_t entry) {
    return (entry & PT_TYPE_MASK) == PT_BLOCK;
}

/* Makes the table under `*entry` (a level-1 or level-2 entry of a root this space may change)
 * private to `space`: a table still shared with the lazy-copy baseline (`base_entry`) is copied,
 * a 2 MB block is split into a level-3 table that reproduces it, and an invalid entry gets an
 * empty table. Returns the table. */
static int address_space_private_table(struct memory_address_space* space, uint64_t* entry,
                                       uint64_t base_entry, bool shared_with_base,
                                       uint64_t** out_table) {
    uint64_t old = *entry;
    uint64_t* table;
    if (is_table(old)) {
        if (!shared_with_base || !is_table(base_entry) ||
                (old & PT_ADDR_MASK) != (base_entry & PT_ADDR_MASK)) {
            *out_table = table_pointer(old);
            return 0;
        }
        int ret = address_space_alloc_table_page(space, &table);
        if (ret < 0)
            return ret;
        memcpy(table, table_pointer(old), PAGE_SIZE);
    } else if (is_block(old)) {
        int ret = address_space_alloc_table_page(space, &table);
        if (ret < 0)
            return ret;
        uint64_t inherit = old & PTE_INHERIT;
        uint64_t base = old & PT_ADDR_MASK;
        for (unsigned int i = 0; i < 512; i++)
            table[i] = (base + i * PAGE_SIZE) | PT_PAGE | inherit;
    } else {
        int ret = address_space_alloc_table_page(space, &table);
        if (ret < 0)
            return ret;
    }
    /* the new table's entries before the entry that points at it (see l3_from_block) */
    __asm__ volatile("dsb ishst" ::: "memory");
    *entry = (memory_frame_from_pointer(table) & PT_ADDR_MASK) | PT_TABLE;
    __asm__ volatile("dsb ishst" ::: "memory");
    *out_table = table;
    return 0;
}

/* The level-3 entry of `addr` in `space`. With `make_private`, every table on the way is made
 * private to the space (copied, split or created); without it, an address without a level-3
 * table (unmapped, or inside a 2 MB block) is -PAL_ERROR_INVAL. */
static int address_space_get_leaf_entry(struct memory_address_space* space, uintptr_t addr,
                                        bool make_private, uint64_t** out_entry) {
    if (!space || !space->root || !space->root_phys || addr >= (1UL << 39) ||
            !IS_ALIGNED(addr, PAGE_SIZE))
        return -PAL_ERROR_INVAL;

    size_t i1 = PT_INDEX_L1(addr);
    size_t i2 = PT_INDEX_L2(addr);
    uint64_t* base_root = space->base_root;
    uint64_t base_l1e = base_root ? base_root[i1] : 0;

    uint64_t* l2;
    if (make_private) {
        int ret = address_space_private_table(space, &space->root[i1], base_l1e,
                                              base_root != NULL, &l2);
        if (ret < 0)
            return ret;
    } else {
        if (!is_table(space->root[i1]))
            return -PAL_ERROR_INVAL;
        l2 = table_pointer(space->root[i1]);
    }

    uint64_t* l3;
    if (make_private) {
        uint64_t base_l2e = is_table(base_l1e) ? table_pointer(base_l1e)[i2] : 0;
        int ret = address_space_private_table(space, &l2[i2], base_l2e, base_root != NULL, &l3);
        if (ret < 0)
            return ret;
    } else {
        if (!is_table(l2[i2]))
            return -PAL_ERROR_INVAL;
        l3 = table_pointer(l2[i2]);
    }

    *out_entry = &l3[PT_INDEX_L3(addr)];
    return 0;
}

/* The level-3 entry that `space` uses for `addr`, without changing anything: a page of a 2 MB
 * block is reported as the level-3 entry the block implies. 0 if nothing maps the address. */
static uint64_t address_space_read_leaf(const struct memory_address_space* space,
                                        uintptr_t addr) {
    if (!space || !space->root || addr >= (1UL << 39))
        return 0;
    uint64_t l1e = space->root[PT_INDEX_L1(addr)];
    if (!is_table(l1e))
        return 0;
    uint64_t l2e = table_pointer(l1e)[PT_INDEX_L2(addr)];
    if (is_block(l2e))
        return ((l2e & PT_ADDR_MASK) + (addr & (PAGE_2M_SIZE - 1) & ~(PAGE_SIZE - 1UL)))
               | PT_PAGE | (l2e & PTE_INHERIT & ~PT_TYPE_MASK);
    if (!is_table(l2e))
        return 0;
    return __atomic_load_n(&table_pointer(l2e)[PT_INDEX_L3(addr)], __ATOMIC_ACQUIRE);
}

static bool leaf_valid(uint64_t entry) {
    return (entry & PT_TYPE_MASK) == PT_PAGE;
}

/* Whether a leaf holds a reference on its frame: every pool frame mapped at a user address does;
 * the identity mapping of the pool itself (in the boot root) does not. */
static bool leaf_holds_reference(uintptr_t va, uint64_t entry) {
    uintptr_t frame = entry & PT_ADDR_MASK;
    return is_frame_pool_frame(frame) && frame != va;
}

/* Writes a leaf of `space`; a valid leaf that changes its output address goes through an invalid
 * entry first (break-before-make). Local TLB maintenance only (when `space` is current); range
 * operations flush the other vCPUs once afterwards. */
static void address_space_write_leaf_bbm(struct memory_address_space* space, uint64_t* entry,
                                         uintptr_t va, uint64_t value, bool remote_bbm) {
    uint64_t old = *entry;
    bool current = current_ttbr0() == space->root_phys;
    if (leaf_valid(old) && leaf_valid(value) && (old & PT_ADDR_MASK) != (value & PT_ADDR_MASK)) {
        __atomic_store_n(entry, 0, __ATOMIC_RELEASE);
        if (current)
            tlb_flush_page_local(va);
        if (remote_bbm && g_num_cpus > 1)
            (void)memory_address_space_flush_range(va, PAGE_SIZE);
    }
    __atomic_store_n(entry, value, __ATOMIC_RELEASE);
    if (current)
        tlb_flush_page_local(va);
    else
        __asm__ volatile("dsb ishst" ::: "memory");
}

static void address_space_write_leaf(struct memory_address_space* space, uint64_t* entry,
                                     uintptr_t va, uint64_t value) {
    address_space_write_leaf_bbm(space, entry, va, value, /*remote_bbm=*/true);
}

static uint64_t user_leaf(uintptr_t frame, bool write, bool execute, bool usermode) {
    uint64_t entry = frame | PTE_USER_PAGE;
    if (!usermode)
        entry |= PTE_AP_EL1ONLY;
    else
        entry |= write ? PTE_AP_EL0RW : PTE_AP_BOTHRO;
    if (!execute)
        entry |= PTE_UXN;
    return entry;
}

static uint64_t lazy_leaf(bool write, bool execute) {
    return PTE_LAZY | (write ? PTE_AP_EL0RW : PTE_AP_BOTHRO) | (execute ? 0 : PTE_UXN);
}

int memory_address_space_clone(uint64_t base_root, struct memory_address_space* out_space) {
    if (!base_root || !IS_ALIGNED(base_root, PAGE_SIZE) || !out_space || !g_frame_pool_bitmap)
        return -PAL_ERROR_INVAL;

    memset(out_space, 0, sizeof(*out_space));
    uint64_t* root;
    int ret = address_space_alloc_table_page(out_space, &root);
    if (ret < 0)
        return ret;
    memcpy(root, memory_frame_pointer(base_root), PAGE_SIZE);
    out_space->root = root;
    out_space->root_phys = memory_frame_from_pointer(root);
    out_space->base_root = memory_frame_pointer(base_root);
    return 0;
}

/* Descendants get private copies of every page table (level 1 to 3); their leaves keep the same
 * output addresses, taking a reference on each pool frame. 2 MB blocks (the kernel's and PAL's
 * identity mappings) stay shared as they are. */
static int address_space_copy_table_tree(struct memory_address_space* target, uint64_t* table,
                                         unsigned int level, uintptr_t va_base) {
    for (size_t i = 0; i < 512; i++) {
        uint64_t entry = table[i];
        if (level == 3) {
            if (leaf_holds_reference(va_base + i * PAGE_SIZE, entry))
                memory_frame_get(entry & PT_ADDR_MASK);
            continue;
        }
        if (!is_table(entry))
            continue;

        uint64_t* child;
        int ret = address_space_alloc_table_page(target, &child);
        if (ret < 0) {
            /* entries from `i` on still point to the source's tables: drop them, so that the
             * caller's destroy releases only what this copy referenced */
            memset(&table[i], 0, (512 - i) * sizeof(*table));
            return ret;
        }
        memcpy(child, table_pointer(entry), PAGE_SIZE);
        table[i] = (entry & ~PT_ADDR_MASK) | (memory_frame_from_pointer(child) & PT_ADDR_MASK);
        uintptr_t shift = level == 1 ? 30 : 21;
        ret = address_space_copy_table_tree(target, child, level + 1, va_base + (i << shift));
        if (ret < 0) {
            if (i + 1 < 512)
                memset(&table[i + 1], 0, (512 - i - 1) * sizeof(*table));
            return ret;
        }
    }
    return 0;
}

int memory_address_space_clone_from(const struct memory_address_space* source,
                                    struct memory_address_space* out_space) {
    if (!source || !source->root || !source->root_phys || !out_space || !g_frame_pool_bitmap)
        return -PAL_ERROR_INVAL;
    memset(out_space, 0, sizeof(*out_space));
    uint64_t* root;
    int ret = address_space_alloc_table_page(out_space, &root);
    if (ret < 0)
        return ret;
    memcpy(root, source->root, PAGE_SIZE);
    out_space->root = root;
    out_space->root_phys = memory_frame_from_pointer(root);
    out_space->base_root = NULL;
    ret = address_space_copy_table_tree(out_space, root, 1, 0);
    if (ret < 0)
        memory_address_space_destroy(out_space);
    return ret;
}

/* Drops the leaf references of the tables this space owns. Tables still shared with the boot
 * root (not pool frames) are not descended into: they hold no references. */
static void address_space_put_leaves(uint64_t* table, unsigned int level, uintptr_t va_base) {
    for (size_t i = 0; i < 512; i++) {
        uint64_t entry = table[i];
        uintptr_t frame = entry & PT_ADDR_MASK;
        if (level == 3) {
            if (leaf_holds_reference(va_base + i * PAGE_SIZE, entry))
                memory_frame_put(frame);
            continue;
        }
        if (!is_table(entry) || !is_frame_pool_frame(frame))
            continue;
        uintptr_t shift = level == 1 ? 30 : 21;
        address_space_put_leaves(table_pointer(entry), level + 1, va_base + (i << shift));
    }
}

size_t memory_address_space_destroy(struct memory_address_space* space) {
    if (!space)
        return 0;
    if (space == &g_boot_address_space)
        BUG();
    for (size_t d = 0; space->root_phys && d < OOM_DOOMED_MAX; d++) {
        uintptr_t doomed = space->root_phys;
        __atomic_compare_exchange_n(&g_oom_doomed_roots[d], &doomed, 0, /*weak=*/false,
                                    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
    }
    if (space->root && space->root_phys && g_frame_pool_bitmap)
        address_space_put_leaves(space->root, 1, 0);
    uintptr_t frame = space->owned_frame_head;
    size_t released = 0;
    while (frame) {
        size_t i = (frame - g_frame_pool_start) / PAGE_SIZE;
        uintptr_t next = g_frame_owner_next[i];
        int ret = memory_frame_free(frame);
        if (ret < 0) {
            log_error("Failed to release address-space %s frame %p: %s",
                      g_frame_owner_kind[i] ? "page-table" : "private-data",
                      (void*)frame, pal_strerror(ret));
            BUG();
        }
        released++;
        frame = next;
    }
    memset(space, 0, sizeof(*space));
    return released;
}

int memory_address_space_map(struct memory_address_space* space, uintptr_t addr,
                             uintptr_t frame, bool write, bool execute, bool usermode) {
    if (!space || !space->root || !space->root_phys || addr >= (1UL << 39) ||
            !IS_ALIGNED(addr, PAGE_SIZE) || !IS_ALIGNED(frame, PAGE_SIZE) ||
            (frame & ~PT_ADDR_MASK))
        return -PAL_ERROR_INVAL;

    uint64_t* pte;
    int ret = address_space_get_leaf_entry(space, addr, /*make_private=*/true, &pte);
    if (ret < 0)
        return ret;
    if (execute)
        sync_icache_frame(frame);
    address_space_write_leaf(space, pte, addr, user_leaf(frame, write, execute, usermode));
    return 0;
}

int memory_address_space_map_owned(struct memory_address_space* space, uintptr_t addr,
                                   uintptr_t frame, bool write, bool execute, bool usermode) {
    uint64_t* pte;
    int ret = address_space_get_leaf_entry(space, addr, /*make_private=*/true, &pte);
    if (ret < 0)
        return ret;
    uint64_t old = *pte;
    uintptr_t old_frame = old & PT_ADDR_MASK;
    bool old_ref = leaf_holds_reference(addr, old);
    ret = memory_address_space_map(space, addr, frame, write, execute, usermode);
    if (ret < 0)
        return ret;
    if (old_ref && old_frame != frame) {
        ret = memory_address_space_flush_range(addr, PAGE_SIZE);
        if (ret < 0)
            return ret;
        memory_frame_put(old_frame);
    }
    return 0;
}

int memory_address_space_map_shared_frame(struct memory_address_space* space, uintptr_t addr,
                                          uintptr_t frame, bool write, bool execute) {
    if (!is_frame_pool_frame(frame))
        return -PAL_ERROR_INVAL;
    memory_frame_get(frame);
    int ret = memory_address_space_map_owned(space, addr, frame, write, execute,
                                             /*usermode=*/true);
    if (ret < 0)
        memory_frame_put(frame);
    return ret;
}

int memory_address_space_copy_private_range(struct memory_address_space* source,
                                            struct memory_address_space* target, uintptr_t addr,
                                            size_t size) {
    if (!source || !target || !IS_ALIGNED(addr, PAGE_SIZE) || !size ||
            !IS_ALIGNED(size, PAGE_SIZE) || addr + size < addr)
        return -PAL_ERROR_INVAL;
    if (!g_frame_pool_bitmap)
        return -PAL_ERROR_NOMEM;

    for (uintptr_t va = addr; va < addr + size; va += PAGE_SIZE) {
        uint64_t source_entry = address_space_read_leaf(source, va);
        if (!leaf_valid(source_entry))
            return -PAL_ERROR_INVAL;
        uintptr_t frame;
        int ret = memory_frame_alloc(&frame);
        if (ret < 0)
            return ret;
        memcpy(memory_frame_pointer(frame), memory_frame_pointer(source_entry & PT_ADDR_MASK),
               PAGE_SIZE);
        uint64_t* target_entry;
        ret = address_space_get_leaf_entry(target, va, /*make_private=*/true, &target_entry);
        if (ret < 0) {
            memory_frame_free(frame);
            return ret;
        }
        uint64_t old = *target_entry;
        /* the copy keeps the source's access rights (the PAL's data is EL0-accessible as in
         * the boot root: LibOS code reads g_pal_public_state from EL0 too) */
        if (!(source_entry & PTE_UXN))
            sync_icache_frame(frame);
        address_space_write_leaf(target, target_entry, va,
                                 frame | (source_entry & ~PT_ADDR_MASK & ~PTE_LAZY));
        /* the new leaf owns `frame`; the reference of the replaced leaf goes after the flush */
        if (leaf_holds_reference(va, old)) {
            ret = memory_address_space_flush_range(va, PAGE_SIZE);
            if (ret < 0)
                return ret;
            memory_frame_put(old & PT_ADDR_MASK);
        }
    }
    return memory_address_space_flush_range(addr, size);
}

int memory_address_space_share_range(struct memory_address_space* source,
                                     struct memory_address_space* target, uintptr_t addr,
                                     size_t size) {
    bool changed;
    int ret = memory_address_space_share_range_noflush(source, target, addr, size, &changed);
    if (ret < 0 || !changed)
        return ret;
    return memory_address_space_flush_range(addr, size);
}

int memory_address_space_share_range_noflush(struct memory_address_space* source,
                                             struct memory_address_space* target,
                                             uintptr_t addr, size_t size, bool* out_changed) {
    if (!source || !target || !IS_ALIGNED(addr, PAGE_SIZE) || !size ||
            !IS_ALIGNED(size, PAGE_SIZE) || addr + size < addr || !out_changed)
        return -PAL_ERROR_INVAL;

    /* The boot root's tables are shared with the lazy copies and changed under the boot-root
     * lock. No caller shares into the boot root on AArch64 once the pool exists (PAL thread
     * stacks then come from the frame alias, see pal_common_threading.c), so refuse instead of
     * taking that lock here, where callers hold the scheduler lock with IRQs masked (a holder of
     * the boot-root lock waits for TLB-shootdown acknowledgements). */
    if (target == &g_boot_address_space)
        return -PAL_ERROR_DENIED;

    bool changed = false;
    *out_changed = false;
    for (uintptr_t va = addr; va < addr + size; va += PAGE_SIZE) {
        uint64_t entry = address_space_read_leaf(source, va);
        if (address_space_read_leaf(target, va) == entry)
            continue; /* already the same (often through a table both still share) */
        uint64_t* target_pte;
        int ret = address_space_get_leaf_entry(target, va, /*make_private=*/true, &target_pte);
        if (ret < 0)
            return ret;
        if (*target_pte == entry)
            continue;
        uintptr_t old_frame = *target_pte & PT_ADDR_MASK;
        if (old_frame != (entry & PT_ADDR_MASK)) {
            if (leaf_holds_reference(va, *target_pte)) {
                /* releasing it would need a flush first; no caller replaces owned frames here */
                return -PAL_ERROR_DENIED;
            }
            if (leaf_holds_reference(va, entry))
                memory_frame_get(entry & PT_ADDR_MASK);
        }
        /* no remote break-before-make here: the caller flushes after its batch, and may hold
         * the scheduler lock with IRQs masked, where waiting for acknowledgements deadlocks */
        address_space_write_leaf_bbm(target, target_pte, va, entry, /*remote_bbm=*/false);
        changed = true;
    }
    *out_changed = changed;
    return 0;
}

int memory_address_space_switch(const struct memory_address_space* space) {
    if (!space || !space->root || !space->root_phys || !IS_ALIGNED(space->root_phys, PAGE_SIZE))
        return -PAL_ERROR_INVAL;
    return memory_address_space_switch_root(space->root_phys);
}

/* Installs `root` in TTBR0. Without ASIDs every entry is global, so the old root's translations
 * are dropped from this CPU's TLB (a local invalidation: the other CPUs keep their own roots). */
int memory_address_space_switch_root(uint64_t root) {
    if (!root || !IS_ALIGNED(root, PAGE_SIZE) || (root & ~PT_ADDR_MASK))
        return -PAL_ERROR_INVAL;
    if (current_ttbr0() == root)
        return 0;
    __asm__ volatile("msr ttbr0_el1, %0\n\tisb\n\ttlbi vmalle1\n\tdsb nsh\n\tisb"
                     : : "r"(root) : "memory");
    return 0;
}

static int address_space_check_range(struct memory_address_space* space, void* addr,
                                     size_t size) {
    if (!space || !IS_ALIGNED_PTR(addr, PAGE_SIZE) || !size || !IS_ALIGNED(size, PAGE_SIZE) ||
            (uintptr_t)addr + size < (uintptr_t)addr)
        return -PAL_ERROR_INVAL;
    if (range_outside_dram((uint64_t)addr, size))
        return -PAL_ERROR_INVAL;
    return 0;
}

int memory_address_space_alloc(struct memory_address_space* space, void* addr, size_t size,
                               bool read, bool write, bool execute) {
    /* the boot root (PAL helper threads, the first thread until it switches to its process
     * root) keeps the identity mappings of the single-process VM */
    if (!g_frame_pool_bitmap || space == &g_boot_address_space)
        return memory_alloc(addr, size, read, write, execute);
    int ret = address_space_check_range(space, addr, size);
    if (ret < 0)
        return ret;

    uintptr_t start = (uintptr_t)addr;
    uintptr_t end = start + size;
    if (!read && !write && !execute) {
        /* An inaccessible reservation has no backing: a later mprotect() gives it fresh frames.
         * A pool frame of a previous mapping stays in the invalid entry until after the flush. */
        for (uintptr_t va = start; va < end; va += PAGE_SIZE) {
            uint64_t* entry;
            ret = address_space_get_leaf_entry(space, va, /*make_private=*/true, &entry);
            if (ret < 0)
                return ret;
            uint64_t frame = *entry & PT_ADDR_MASK;
            address_space_write_leaf(space, entry, va,
                                     leaf_holds_reference(va, *entry) ? frame : 0);
        }
        ret = memory_address_space_flush_range(start, size);
        if (ret < 0)
            return ret;
        for (uintptr_t va = start; va < end; va += PAGE_SIZE) {
            uint64_t* entry;
            ret = address_space_get_leaf_entry(space, va, /*make_private=*/false, &entry);
            if (ret < 0)
                return ret;
            if (leaf_holds_reference(va, *entry))
                memory_frame_put(*entry & PT_ADDR_MASK);
            *entry = 0;
        }
        return 0;
    }

    /* Demand-zero paging: install lazy entries; memory_address_space_handle_fault() gives each
     * page a zeroed frame on first access, so a process pays only for the pages it touches. */
    uint64_t lazy = lazy_leaf(write, execute);
    for (uintptr_t va = start; va < end; va += PAGE_SIZE) {
        /* create all page tables first, so that a failure changes no mapping */
        uint64_t* entry;
        ret = address_space_get_leaf_entry(space, va, /*make_private=*/true, &entry);
        if (ret < 0)
            return ret;
    }
    bool replaced_frames = false;
    for (uintptr_t va = start; va < end; va += PAGE_SIZE) {
        uint64_t* entry;
        ret = address_space_get_leaf_entry(space, va, /*make_private=*/false, &entry);
        if (ret < 0)
            return ret;
        if (leaf_holds_reference(va, *entry)) {
            /* keep the frame in an invalid entry until the other vCPUs dropped it */
            address_space_write_leaf(space, entry, va, *entry & ~PT_TYPE_MASK);
            replaced_frames = true;
        } else {
            address_space_write_leaf(space, entry, va, lazy);
        }
    }
    ret = memory_address_space_flush_range(start, size);
    if (ret < 0)
        return ret;
    if (replaced_frames) {
        for (uintptr_t va = start; va < end; va += PAGE_SIZE) {
            uint64_t* entry;
            ret = address_space_get_leaf_entry(space, va, /*make_private=*/false, &entry);
            if (ret < 0)
                return ret;
            if (leaf_holds_reference(va, *entry)) {
                memory_frame_put(*entry & PT_ADDR_MASK);
                *entry = lazy;
            }
        }
    }
    return 0;
}

/* Fault hook: materializes a demand-zero page of the current root. `error_code` uses the x86
 * page-fault bits (FAULT_PRESENT for a permission fault on a valid entry, FAULT_WRITE,
 * FAULT_FETCH); returns false for any fault it does not resolve. Runs with IRQs masked, for
 * faults from EL0 and EL1 alike. */
bool memory_address_space_handle_fault(uintptr_t addr, uint64_t error_code) {
    if (!g_frame_pool_bitmap || (error_code & FAULT_PRESENT) || addr >= (1UL << 39))
        return false;

    uint64_t* root = memory_frame_pointer(current_ttbr0());
    uint64_t l1e = root[PT_INDEX_L1(addr)];
    if (!is_table(l1e))
        return false;
    uint64_t l2e = table_pointer(l1e)[PT_INDEX_L2(addr)];
    if (!is_table(l2e))
        return false;
    uint64_t* pte = &table_pointer(l2e)[PT_INDEX_L3(addr)];
    uint64_t old = __atomic_load_n(pte, __ATOMIC_ACQUIRE);
    if (leaf_valid(old))
        return true; /* another vCPU materialized it meanwhile */
    if (!(old & PTE_LAZY))
        return false;
    if ((error_code & FAULT_WRITE) && (old & PTE_AP_MASK) != PTE_AP_EL0RW)
        return false; /* write to a read-only page: a genuine fault */
    if ((error_code & FAULT_FETCH) && (old & PTE_UXN))
        return false;

    uintptr_t frame;
    if (memory_frame_alloc(&frame) < 0) {
        /* a process that runs out usually faults on page after page: report the first few */
        static uint32_t reported;
        uint32_t n = __atomic_fetch_add(&reported, 1, __ATOMIC_RELAXED);
        if (n < 4)
            log_error("Out of frames for a demand-zero page at 0x%lx%s", addr,
                      n == 3 ? " (further ones not reported)" : "");
        return false;
    }
    if (!(old & PTE_UXN))
        sync_icache_frame(frame);
    uint64_t new = frame | PTE_USER_PAGE | (old & PTE_RIGHTS);
    if (!__atomic_compare_exchange_n(pte, &old, new, /*weak=*/false, __ATOMIC_ACQ_REL,
                                     __ATOMIC_ACQUIRE)) {
        memory_frame_free(frame); /* lost a race; the winner's mapping (or change) stands */
    }
    /* an invalid entry is never cached in a TLB: making the new one visible to this CPU's walks
     * is enough */
    __asm__ volatile("dsb ishst\n\tisb" ::: "memory");
    return true;
}

/* The fallback of memory_address_space_handle_fault() for a fault of PAL code when the pool is
 * empty (see g_oom_sink_frame): maps the sink frame at the demand-zero page of the current root,
 * which must be a process root, and marks that root. Returns false if the fault is not such a
 * page or the sink cannot take another reference. */
bool memory_address_space_handle_oom_fault(uintptr_t addr, uint64_t error_code) {
    if (!g_oom_sink_frame || !g_frame_pool_bitmap || (error_code & FAULT_PRESENT) ||
            addr >= (1UL << 39))
        return false;
    uintptr_t root_phys = current_ttbr0();
    if (root_phys == g_pml4_table_base)
        return false;
    uint64_t* root = memory_frame_pointer(root_phys);
    uint64_t l1e = root[PT_INDEX_L1(addr)];
    if (!is_table(l1e))
        return false;
    uint64_t l2e = table_pointer(l1e)[PT_INDEX_L2(addr)];
    if (!is_table(l2e))
        return false;
    uint64_t* pte = &table_pointer(l2e)[PT_INDEX_L3(addr)];
    uint64_t old = __atomic_load_n(pte, __ATOMIC_ACQUIRE);
    if (leaf_valid(old))
        return true;
    if (!(old & PTE_LAZY))
        return false;
    if ((error_code & FAULT_WRITE) && (old & PTE_AP_MASK) != PTE_AP_EL0RW)
        return false;

    size_t i = (g_oom_sink_frame - g_frame_pool_start) / PAGE_SIZE;
    uint64_t flags = frame_pool_lock();
    if (g_frame_refcount[i] == UINT16_MAX) {
        frame_pool_unlock(flags);
        return false;
    }
    g_frame_refcount[i]++; /* the leaf's reference, dropped when the leaf goes */
    frame_pool_unlock(flags);

    bool marked = false;
    for (size_t d = 0; d < OOM_DOOMED_MAX && !marked; d++) {
        uintptr_t expected = 0;
        marked = __atomic_load_n(&g_oom_doomed_roots[d], __ATOMIC_ACQUIRE) == root_phys ||
                 __atomic_compare_exchange_n(&g_oom_doomed_roots[d], &expected, root_phys,
                                             /*weak=*/false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
    }
    if (!marked) {
        memory_frame_put(g_oom_sink_frame);
        return false;
    }
    uint64_t new = g_oom_sink_frame | PTE_USER_PAGE | (old & PTE_RIGHTS) | PTE_UXN;
    if (!__atomic_compare_exchange_n(pte, &old, new, /*weak=*/false, __ATOMIC_ACQ_REL,
                                     __ATOMIC_ACQUIRE))
        memory_frame_put(g_oom_sink_frame);
    __asm__ volatile("dsb ishst\n\tisb" ::: "memory");
    return true;
}

/* Whether the current root was marked by memory_address_space_handle_oom_fault(). */
bool memory_address_space_current_doomed(void) {
    uintptr_t root_phys = current_ttbr0();
    for (size_t d = 0; d < OOM_DOOMED_MAX; d++)
        if (__atomic_load_n(&g_oom_doomed_roots[d], __ATOMIC_ACQUIRE) == root_phys)
            return true;
    return false;
}

int memory_address_space_translate(uintptr_t addr, uintptr_t* out_phys) {
    if (!g_frame_pool_bitmap || addr >= (1UL << 39))
        return -PAL_ERROR_INVAL;
    uint64_t* root = memory_frame_pointer(current_ttbr0());
    uint64_t l1e = root[PT_INDEX_L1(addr)];
    if (!is_table(l1e))
        return -PAL_ERROR_INVAL;
    uint64_t l2e = __atomic_load_n(&table_pointer(l1e)[PT_INDEX_L2(addr)], __ATOMIC_ACQUIRE);
    if (is_block(l2e)) {
        *out_phys = (l2e & PT_ADDR_MASK & ~(PAGE_2M_SIZE - 1)) | (addr & (PAGE_2M_SIZE - 1));
        return 0;
    }
    if (!is_table(l2e))
        return -PAL_ERROR_INVAL;
    uint64_t pte = __atomic_load_n(&table_pointer(l2e)[PT_INDEX_L3(addr)], __ATOMIC_ACQUIRE);
    if (!leaf_valid(pte))
        return -PAL_ERROR_INVAL;
    *out_phys = (pte & PT_ADDR_MASK) | (addr & (PAGE_SIZE - 1));
    return 0;
}

int memory_address_space_free(struct memory_address_space* space, void* addr, size_t size) {
    /* the boot root (PAL helper threads, the first thread until it switches to its process
     * root) keeps the identity mappings of the single-process VM */
    if (!g_frame_pool_bitmap || space == &g_boot_address_space)
        return memory_free(addr, size);
    int ret = address_space_check_range(space, addr, size);
    if (ret < 0)
        return ret;

    uintptr_t start = (uintptr_t)addr;
    uintptr_t end = start + size;
    for (uintptr_t va = start; va < end; va += PAGE_SIZE) {
        uint64_t* entry;
        ret = address_space_get_leaf_entry(space, va, /*make_private=*/true, &entry);
        if (ret < 0)
            return ret;
        /* keep a referenced frame in the invalid entry for the second pass */
        address_space_write_leaf(space, entry, va,
                                 leaf_holds_reference(va, *entry) ? *entry & PT_ADDR_MASK : 0);
    }
    ret = memory_address_space_flush_range(start, size);
    if (ret < 0)
        return ret;

    /* Do not return a frame to the pool until remote vCPUs have dropped cached mappings to it. */
    for (uintptr_t va = start; va < end; va += PAGE_SIZE) {
        uint64_t* entry;
        ret = address_space_get_leaf_entry(space, va, /*make_private=*/false, &entry);
        if (ret < 0)
            return ret;
        if (leaf_holds_reference(va, *entry))
            memory_frame_put(*entry & PT_ADDR_MASK);
        *entry = 0;
    }
    return 0;
}

int memory_address_space_protect(struct memory_address_space* space, void* addr, size_t size,
                                 bool read, bool write, bool execute) {
    /* the boot root (PAL helper threads, the first thread until it switches to its process
     * root) keeps the identity mappings of the single-process VM */
    if (!g_frame_pool_bitmap || space == &g_boot_address_space)
        return memory_protect(addr, size, read, write, execute);
    int ret = address_space_check_range(space, addr, size);
    if (ret < 0)
        return ret;

    bool none = !read && !write && !execute;
    uintptr_t start = (uintptr_t)addr;
    uintptr_t end = start + size;
    for (uintptr_t va = start; va < end; va += PAGE_SIZE) {
        uint64_t* entry;
        ret = address_space_get_leaf_entry(space, va, /*make_private=*/true, &entry);
        if (ret < 0)
            return ret;
        uint64_t old = *entry;
        uintptr_t frame = old & PT_ADDR_MASK;
        uint64_t value;
        if (!leaf_valid(old) && !leaf_holds_reference(va, old)) {
            /* an unbacked page (never touched, or a PROT_NONE reservation) stays demand-zero */
            value = none ? 0 : lazy_leaf(write, execute);
        } else if (none) {
            value = frame; /* kept in an invalid entry, given back by a later mprotect() */
        } else {
            if (execute)
                sync_icache_frame(frame);
            value = user_leaf(frame, write, execute, /*usermode=*/true);
        }
        address_space_write_leaf(space, entry, va, value);
    }
    return memory_address_space_flush_range(start, size);
}
