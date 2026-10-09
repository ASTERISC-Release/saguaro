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
#include "kernel_memory.h"

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
#define PT_POOL_TABLES 512
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

static uint64_t* pt_alloc_table(const char* what) {
    if (g_pt_pool_used >= PT_POOL_TABLES) {
        /* no allocation during an exception-safe context below; boot-early failure only */
        INIT_FAIL("out of page-table pool while creating %s table", what);
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
    __asm__ volatile("tlbi vmalle1\n\tdsb ish\n\tisb" ::: "memory");
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
    for (unsigned i = 0; i < 512; i++) {
        l3[i] = PT_PAGE | base + i * PAGE_SIZE + inherit;
    }
    *l2e = ((uint64_t)(uintptr_t)l3 & PT_ADDR_MASK) | PT_TABLE;
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
            return create ? &l3_from_block(l2e)[PT_INDEX_L3(va)] : l2e;
        }
        if (create && va >= g_dram_base && va < g_dram_end) {
            /* the whole 2MB block was marked off: reinstate it, then split */
            *l2e = (va & ~(0x00200000ULL - 1)) | PTE_DATA;
            return &l3_from_block(l2e)[PT_INDEX_L3(va)];
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

int memory_alloc(void* addr, size_t size, bool read, bool write, bool execute) {
    if (!read && !write && !execute) {
        return memory_mark_pages_off((uint64_t)addr, size);
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

    /* Install. The switch is transparent: the tables describe the same identity mapping, the
     * current stack is PTE_DATA (.bss below the rx segment, EL1 rw with PAN cleared), and the
     * next fetch is PAL text -- PTE_EXEC, EL1 r+x -- so nothing between `msr` and `isb` depends
     * on the old table. Interrupts are still masked (unmasked only after gic_init), and the
     * vector table itself will be fetched from PTE_EXEC pages once installed. */
    g_page_table_root = l1;
    __asm__ volatile(
        "msr ttbr0_el1, %0\n\t"
        "isb\n\t"
        "tlbi vmalle1\n\t"
        "dsb ish\n\t"
        "isb\n\t"
        ::"r"(l1)
        : "memory");

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
