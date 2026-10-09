/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2023 Intel Corporation */

/*
 * aarch64 counterpart of vm-common/kernel_memory.c.
 *
 * Milestone-B status: BOOT-STUB. The x86 file builds 4-level x86 page tables covering the whole
 * address space with fine-grained permission tracking (PAT/WC, per-page RWX toggling, TLB
 * shootdown protocol). The aarch64 equivalents (VMSA 4KB-granule L1/L2/L3 tables, MAIR attribute
 * indices for WC memory, `tlbi` broadcast) land with the bring-up commits; until then this file
 * provides the same API surface on top of the bootloader's identity map so that every consumer
 * (virtio drivers, PAL memory allocator, file driver) links and runs with the kernel's "all
 * memory identity-mapped RWX" boot state.
 *
 * Everything that is honest bookkeeping rather than page-table manipulation (shared-virtqueue
 * region bump allocator, memory range reporting for preloaded ranges) is already real.
 */

#include <stdbool.h>
#include <stdint.h>

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
 * virtio-pci devices, so a simple bump allocator over a static window suffices until the real
 * page-table machinery (and with it, explicit non-cacheable shared pages) arrives. */
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
     * their rings); reclaiming the middle of the window is a Milestone-B follow-up like the x86
     * "free shared region" reclamation that also relies on allocation patterns not racing. */
    return 0;
}

int memory_find_page_table_entry(uint64_t addr, uint64_t** out_pte_addr) {
    __UNUSED(addr);
    __UNUSED(out_pte_addr);
    /* TODO(Milestone-B): walk VMSA L1->L2->L3 tables once kernel_pagetables exists; used only
     * for #PF diagnostics on x86. */
    return -PAL_ERROR_NOTIMPLEMENTED;
}

int memory_pagetables_init(void* memory_address_end, bool current_page_tables_cover_1gb) {
    __UNUSED(current_page_tables_cover_1gb);
    /* TODO(Milestone-B): build full VMSA tables covering [0, memory_address_end):
     *   - [0, 0x08000000): normal WB (low DRAM/flash reserved areas)
     *   - [0x08000000, 0x0a000000): device-nGnRE (GIC dist/redist, PL011, fw_cfg)
     *   - [0x0a000000, 0x10000000): device-nGnRE (virtio-mmio MMIO window)
     *   - [0x10000000, 0x3f000000): normal WB (PCI MMIO low window / first DRAM GBs)
     *   - [0x3f000000, 0x40000000): device-nGnRE (ECAM)
     *   - DRAM from 0x40000000: normal WB, user-accessible
     * Until then the bootloader identity map (4GB, same attribute layout at 1GB/32MB granularity)
     * is what the kernel actually runs on, which is sufficient for bring-up. */
    __UNUSED(memory_address_end);
    return 0;
}

void memory_pat_init_for_wc(void) {
    /* No PAT register on aarch64. Write-combining is an MAIR attribute index chosen per-page;
     * becomes real together with memory_pagetables_init(). */
}

int memory_mark_pages_on(uint64_t addr, size_t size, bool write, bool execute, bool usermode) {
    __UNUSED(addr);
    __UNUSED(size);
    __UNUSED(write);
    __UNUSED(execute);
    __UNUSED(usermode);
    return 0; /* boot state: all identity-mapped pages are already RWX at EL0 */
}

int memory_mark_pages_off(uint64_t addr, size_t size) {
    __UNUSED(addr);
    __UNUSED(size);
    return 0; /* TODO(Milestone-B): clear table bits + tlbi broadcast */
}

int memory_mark_pages_strong_uncacheable(uint64_t addr, size_t size, bool mark) {
    __UNUSED(addr);
    __UNUSED(size);
    __UNUSED(mark);
    return 0; /* TODO(Milestone-B): switch MAIR index to Device-nGnRnE per page */
}

int memory_mark_pages_write_combined(uint64_t addr, size_t size, bool mark) {
    __UNUSED(addr);
    __UNUSED(size);
    __UNUSED(mark);
    return 0; /* TODO(Milestone-B): switch MAIR index to WC per page */
}

int memory_alloc(void* addr, size_t size, bool read, bool write, bool execute) {
    __UNUSED(addr);
    __UNUSED(size);
    __UNUSED(read);
    __UNUSED(write);
    __UNUSED(execute);
    return 0; /* pages are already present and RWX in the boot map */
}

int memory_protect(void* addr, size_t size, bool read, bool write, bool execute) {
    __UNUSED(addr);
    __UNUSED(size);
    __UNUSED(read);
    __UNUSED(write);
    __UNUSED(execute);
    return 0; /* TODO(Milestone-B): flip table bits + tlbi */
}

int memory_free(void* addr, size_t size) {
    __UNUSED(addr);
    __UNUSED(size);
    return 0;
}

int memory_tighten_permissions(void) {
    /* TODO(Milestone-B): make PAL text RO+X, rodata RO, remove W+X everywhere. Not yet: the
     * identity boot map is RWX and pal_main()'s zero-out pass is skipped on aarch64 until
     * page-level protection exists. */
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
