/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2023 Intel Corporation */

/*
 * Memory helpers.
 *
 * Notes on multi-core synchronization:
 *   - memory_get_shared_region()/memory_free_shared_region() are used on init, no sync required
 *   - g_pml4_table_base, page tables, Address Sanitizer are set on init, no sync required
 *   - memory_alloc(), memory_protect() and memory_free() rely on LibOS synchronization and thus do
 *     not require additional PAL-level sync
 *   - all other funcs are used only at init, no sync required
 */

#include <stdint.h>

#include "api.h"
#include "asan.h"
#include "pal_error.h"

#include "kernel_debug.h"
#include "kernel_interrupts.h"
#include "kernel_memory.h"
#include "kernel_multicore.h"
#include "kernel_virtio.h"

#ifdef __x86_64__
/* Process-private backing frames come from an explicitly reserved, supervisor-only interval. The
 * pool is opt-in until address spaces consume these frames; single-process guests keep the full
 * existing memory quota. */
static uintptr_t g_frame_pool_start;
static size_t g_frame_pool_size;
static size_t g_frame_pool_next;
static size_t g_frame_alias_mapped_size;
static uint8_t* g_frame_pool_bitmap;
static uintptr_t* g_frame_owner_next;
static uint8_t* g_frame_owner_kind;
static spinlock_t g_frame_pool_lock = INIT_SPINLOCK_UNLOCKED;

/* A supervisor-only direct-map window gives every root stable access to physical frames and
 * page tables, even when a process remaps their identity addresses. */
#define FRAME_POOL_ALIAS_BASE 0xffff800000000000UL
#define FRAME_POOL_ALIAS_SIZE (512UL * 1024 * 1024 * 1024)
#define PAGE_2M_SIZE (2UL * 1024 * 1024)
#define PTE_PRESENT 0x001UL
#define PTE_WRITE   0x002UL
#define PTE_HUGE    0x080UL
#define PTE_NX      (1UL << 63)

static struct memory_address_space g_boot_address_space;

void* memory_frame_pointer(uintptr_t frame) {
    if (g_frame_alias_mapped_size && frame < g_frame_alias_mapped_size)
        return (void*)(FRAME_POOL_ALIAS_BASE + frame);
    return (void*)frame;
}

static uintptr_t memory_frame_from_pointer(const void* pointer) {
    uintptr_t addr = (uintptr_t)pointer;
    if (g_frame_alias_mapped_size && addr >= FRAME_POOL_ALIAS_BASE &&
            addr < FRAME_POOL_ALIAS_BASE + g_frame_alias_mapped_size)
        return addr - FRAME_POOL_ALIAS_BASE;
    return addr;
}

static int memory_frame_pool_map_alias(size_t memory_size) {
    if (!memory_size || memory_size > FRAME_POOL_ALIAS_SIZE)
        return -PAL_ERROR_INVAL;
    size_t mapped_size = ALIGN_UP(memory_size, PAGE_2M_SIZE);
    if (mapped_size > FRAME_POOL_ALIAS_SIZE)
        return -PAL_ERROR_INVAL;

    uint64_t* pdpt = memory_get_shared_region(PAGE_SIZE);
    size_t pd_count = UDIV_ROUND_UP(mapped_size, 1024UL * 1024 * 1024);
    uint64_t* pd_tables = memory_get_shared_region(pd_count * PAGE_SIZE);
    if (!pdpt || !pd_tables)
        return -PAL_ERROR_NOMEM;

    for (size_t pdpt_idx = 0; pdpt_idx < pd_count; pdpt_idx++) {
        uint64_t* pd = (uint64_t*)((uintptr_t)pd_tables + pdpt_idx * PAGE_SIZE);
        pdpt[pdpt_idx] = (uintptr_t)pd | PTE_PRESENT | PTE_WRITE;
        size_t remaining = (mapped_size - pdpt_idx * 1024UL * 1024 * 1024) / PAGE_2M_SIZE;
        size_t pages_in_pd = MIN(512UL, remaining);
        for (size_t i = 0; i < pages_in_pd; i++) {
            uintptr_t physical = pdpt_idx * 1024UL * 1024 * 1024 + i * PAGE_2M_SIZE;
            pd[i] = physical | PTE_PRESENT | PTE_WRITE | PTE_HUGE | PTE_NX;
        }
    }

    size_t pml4_idx = (FRAME_POOL_ALIAS_BASE >> 39) & 0x1ff;
    uint64_t* pml4 = (uint64_t*)g_pml4_table_base;
    if (pml4[pml4_idx] & PTE_PRESENT)
        return -PAL_ERROR_DENIED;
    pml4[pml4_idx] = (uintptr_t)pdpt | PTE_PRESENT | PTE_WRITE;
    g_frame_alias_mapped_size = mapped_size;
    g_frame_pool_bitmap = memory_frame_pointer((uintptr_t)g_frame_pool_bitmap);
    g_frame_owner_next = memory_frame_pointer((uintptr_t)g_frame_owner_next);
    g_frame_owner_kind = memory_frame_pointer((uintptr_t)g_frame_owner_kind);
    g_boot_address_space.root = memory_frame_pointer(g_pml4_table_base);
    g_boot_address_space.root_phys = g_pml4_table_base;
    g_boot_address_space.base_root = g_boot_address_space.root;
    return send_invalidate_tlb_ipi_and_wait(NULL, 0, /*invalidate_on_this_cpu=*/true);
}

int memory_frame_pool_init(uintptr_t addr, size_t size) {
    if (g_frame_pool_bitmap || !size || !IS_ALIGNED_POW2(addr, PAGE_SIZE) ||
            !IS_ALIGNED_POW2(size, PAGE_SIZE) || addr < 1024UL * 1024 * 1024 ||
            addr + size < addr || addr + size > (uintptr_t)g_pal_public_state.memory_address_end)
        return -PAL_ERROR_INVAL;

    size_t frame_count = size / PAGE_SIZE;
    size_t bitmap_size = UDIV_ROUND_UP(frame_count, 8);
    uint8_t* bitmap = memory_get_shared_region(bitmap_size);
    if (!bitmap)
        return -PAL_ERROR_NOMEM;

    int ret = memory_mark_pages_on(addr, size, /*write=*/true, /*execute=*/false,
                                   /*usermode=*/false);
    if (ret < 0)
        return ret;

    memset(bitmap, 0, bitmap_size);
    g_frame_pool_start = addr;
    g_frame_pool_size = size;
    g_frame_pool_bitmap = bitmap;
    size_t owner_next_size = frame_count * sizeof(*g_frame_owner_next);
    size_t owner_kind_size = frame_count * sizeof(*g_frame_owner_kind);
    g_frame_owner_next = memory_get_shared_region(owner_next_size);
    g_frame_owner_kind = memory_get_shared_region(owner_kind_size);
    if (!g_frame_owner_next || !g_frame_owner_kind)
        return -PAL_ERROR_NOMEM;
    memset(g_frame_owner_next, 0, owner_next_size);
    memset(g_frame_owner_kind, 0, owner_kind_size);
    ret = memory_frame_pool_map_alias((uintptr_t)g_pal_public_state.memory_address_end);
    if (ret < 0)
        return ret;
    log_always("Private frame pool: [%p, %p), %zu frames", (void*)addr, (void*)(addr + size),
               frame_count);
    return 0;
}

int memory_frame_alloc(uintptr_t* out_frame) {
    if (!out_frame || !g_frame_pool_bitmap)
        return -PAL_ERROR_INVAL;

    spinlock_lock_disable_irq(&g_frame_pool_lock);
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
#ifdef ASAN
        asan_unpoison_region(frame, PAGE_SIZE);
#endif
        _real_memset(memory_frame_pointer(frame), 0, PAGE_SIZE);
        *out_frame = frame;
        spinlock_unlock_enable_irq(&g_frame_pool_lock);
        return 0;
    }
    spinlock_unlock_enable_irq(&g_frame_pool_lock);
    return -PAL_ERROR_NOMEM;
}

int memory_frame_free(uintptr_t frame) {
    if (!g_frame_pool_bitmap || frame < g_frame_pool_start ||
            frame >= g_frame_pool_start + g_frame_pool_size ||
            !IS_ALIGNED_POW2(frame, PAGE_SIZE))
        return -PAL_ERROR_INVAL;

    size_t i = (frame - g_frame_pool_start) / PAGE_SIZE;
    uint8_t mask = 1U << (i % 8);
    spinlock_lock_disable_irq(&g_frame_pool_lock);
    if (!(g_frame_pool_bitmap[i / 8] & mask)) {
        spinlock_unlock_enable_irq(&g_frame_pool_lock);
        return -PAL_ERROR_INVAL;
    }
    g_frame_pool_bitmap[i / 8] &= ~mask;
    g_frame_owner_next[i] = 0;
    g_frame_owner_kind[i] = 0;
    if (i < g_frame_pool_next)
        g_frame_pool_next = i;
#ifdef ASAN
    asan_poison_region(frame, PAGE_SIZE, ASAN_POISON_USER);
#endif
    spinlock_unlock_enable_irq(&g_frame_pool_lock);
    return 0;
}

size_t memory_frame_allocated_count(void) {
    if (!g_frame_pool_bitmap)
        return 0;
    size_t count = 0;
    size_t frame_count = g_frame_pool_size / PAGE_SIZE;
    spinlock_lock_disable_irq(&g_frame_pool_lock);
    for (size_t i = 0; i < frame_count; i++)
        count += !!(g_frame_pool_bitmap[i / 8] & (1U << (i % 8)));
    spinlock_unlock_enable_irq(&g_frame_pool_lock);
    return count;
}

/* Mapping helpers invalidate the current vCPU while changing individual entries. Range operations
 * call this once after updating all entries so other vCPUs cannot retain stale translations for
 * threads sharing the address space. */
int memory_address_space_flush_range(uintptr_t addr, size_t size) {
    if (g_num_cpus <= 1)
        return 0;
    return send_invalidate_tlb_ipi_and_wait((void*)addr, size,
                                           /*invalidate_on_this_cpu=*/true);
}

static int address_space_get_leaf_entry(struct memory_address_space* space, uintptr_t addr,
                                        bool make_private, uint64_t** out_entry);

static int address_space_track_frame(struct memory_address_space* space, uintptr_t frame,
                                    bool is_page_table) {
    if (!g_frame_pool_bitmap)
        return 0;
    if (frame < g_frame_pool_start || frame >= g_frame_pool_start + g_frame_pool_size ||
            !IS_ALIGNED_POW2(frame, PAGE_SIZE))
        return -PAL_ERROR_INVAL;
    size_t i = (frame - g_frame_pool_start) / PAGE_SIZE;
    g_frame_owner_next[i] = space->owned_frame_head;
    g_frame_owner_kind[i] = is_page_table;
    space->owned_frame_head = frame;
    return 0;
}

static int address_space_alloc_table_page(struct memory_address_space* space, uint64_t** out_page) {
    if (!g_frame_pool_bitmap) {
        *out_page = memory_get_shared_region(PAGE_SIZE);
        return *out_page ? 0 : -PAL_ERROR_NOMEM;
    }

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

/* Clone a child table only the first time an address-space map operation reaches it. The parent
 * and child can therefore share all untouched identity-map tables while writes to a private leaf
 * remain local to the child root. */
static int address_space_get_private_child(uint64_t* entry, uint64_t base_entry,
                                           struct memory_address_space* space,
                                           uint64_t** out_child) {
    const uint64_t addr_mask = 0x00000ffffffff000UL;
    if (!(*entry & 1) || !(*entry & addr_mask) || !(base_entry & 1) ||
            !((base_entry) & addr_mask))
        return -PAL_ERROR_INVAL;

    if ((*entry & addr_mask) == (base_entry & addr_mask)) {
        uint64_t* copy;
        int ret = address_space_alloc_table_page(space, &copy);
        if (ret < 0)
            return ret;
        memcpy(copy, memory_frame_pointer(*entry & addr_mask), PAGE_SIZE);
        *entry = (*entry & ~addr_mask) | (memory_frame_from_pointer(copy) & addr_mask);
    }

    *out_child = memory_frame_pointer(*entry & addr_mask);
    return 0;
}

int memory_address_space_clone(uint64_t base_root, struct memory_address_space* out_space) {
    if (!base_root || !IS_ALIGNED_POW2(base_root, PAGE_SIZE) || !out_space)
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

/* Descendants get private copies of every page-table page. Their leaves keep the same physical
 * mappings; process-private memory is copied separately by the caller. */
static int address_space_copy_table_tree(struct memory_address_space* target, uint64_t* table,
                                         unsigned int level) {
    const uint64_t addr_mask = 0x00000ffffffff000UL;
    size_t alias_pml4_idx = (FRAME_POOL_ALIAS_BASE >> 39) & 0x1ff;
    for (size_t i = 0; i < 512; i++) {
        uint64_t entry = table[i];
        if (!(entry & PTE_PRESENT) || level == 1 ||
                ((level == 2 || level == 3) && (entry & PTE_HUGE)) ||
                (level == 4 && i == alias_pml4_idx))
            continue;

        uint64_t* child;
        int ret = address_space_alloc_table_page(target, &child);
        if (ret < 0)
            return ret;
        memcpy(child, memory_frame_pointer(entry & addr_mask), PAGE_SIZE);
        table[i] = (entry & ~addr_mask) | (memory_frame_from_pointer(child) & addr_mask);
        ret = address_space_copy_table_tree(target, child, level - 1);
        if (ret < 0)
            return ret;
    }
    return 0;
}

int memory_address_space_clone_from(const struct memory_address_space* source,
                                    struct memory_address_space* out_space) {
    if (!source || !source->root || !source->root_phys || !out_space)
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
    ret = address_space_copy_table_tree(out_space, root, 4);
    if (ret < 0)
        memory_address_space_destroy(out_space);
    return ret;
}

size_t memory_address_space_destroy(struct memory_address_space* space) {
    if (!space)
        return 0;
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

int memory_address_space_copy_private_range(struct memory_address_space* source,
                                            struct memory_address_space* target, uintptr_t addr,
                                            size_t size) {
    if (!source || !target || !IS_ALIGNED_POW2(addr, PAGE_SIZE) || !size ||
            !IS_ALIGNED_POW2(size, PAGE_SIZE) || addr + size < addr)
        return -PAL_ERROR_INVAL;
    if (!g_frame_pool_bitmap)
        return -PAL_ERROR_NOMEM;

    for (uintptr_t va = addr; va < addr + size; va += PAGE_SIZE) {
        uint64_t* source_entry;
        int ret = address_space_get_leaf_entry(source, va,
                                               /*make_private=*/false, &source_entry);
        if (ret < 0 || !(*source_entry & 1))
            return ret < 0 ? ret : -PAL_ERROR_INVAL;
        uintptr_t frame;
        ret = memory_frame_alloc(&frame);
        if (ret < 0)
            return ret;
        memcpy(memory_frame_pointer(frame),
               memory_frame_pointer(*source_entry & 0x00000ffffffff000UL), PAGE_SIZE);
        ret = address_space_track_frame(target, frame, /*is_page_table=*/false);
        if (ret < 0) {
            memory_frame_free(frame);
            return ret;
        }
        ret = memory_address_space_map(target, va, frame, /*write=*/true, /*execute=*/false,
                                       /*usermode=*/false);
        if (ret < 0)
            return ret;
    }
    return memory_address_space_flush_range(addr, size);
}

static int address_space_get_leaf_entry(struct memory_address_space* space, uintptr_t addr,
                                        bool make_private, uint64_t** out_entry) {
    const uint64_t addr_mask = 0x00000ffffffff000UL;
    if (!space || !space->root || !space->root_phys ||
            addr >= 512UL * 1024 * 1024 * 1024 || !IS_ALIGNED_POW2(addr, PAGE_SIZE))
        return -PAL_ERROR_INVAL;

    size_t pml4_idx = (addr >> 39) & 0x1ff;
    size_t pdpt_idx = (addr >> 30) & 0x1ff;
    size_t pd_idx = (addr >> 21) & 0x1ff;
    size_t pt_idx = (addr >> 12) & 0x1ff;
    uint64_t* base_root = space->base_root;
    uint64_t* base_pdpt = NULL;
    uint64_t* base_pd = NULL;
    uint64_t* pdpt;
    int ret = 0;
    if (base_root && make_private)
        ret = address_space_get_private_child(&space->root[pml4_idx], base_root[pml4_idx],
                                              space, &pdpt);
    else if (!(space->root[pml4_idx] & PTE_PRESENT))
        return -PAL_ERROR_INVAL;
    else
        pdpt = memory_frame_pointer(space->root[pml4_idx] & addr_mask);
    if (ret < 0)
        return ret;

    if (base_root) {
        if (!(base_root[pml4_idx] & PTE_PRESENT))
            return -PAL_ERROR_INVAL;
        base_pdpt = memory_frame_pointer(base_root[pml4_idx] & addr_mask);
    }
    if ((pdpt[pdpt_idx] | (base_pdpt ? base_pdpt[pdpt_idx] : 0)) & PTE_HUGE)
        return -PAL_ERROR_INVAL; /* splitting a 1-GiB page is not implemented */
    uint64_t* pd;
    if (base_root && make_private) {
        ret = address_space_get_private_child(&pdpt[pdpt_idx], base_pdpt[pdpt_idx], space, &pd);
    } else {
        if (!(pdpt[pdpt_idx] & PTE_PRESENT))
            return -PAL_ERROR_INVAL;
        pd = memory_frame_pointer(pdpt[pdpt_idx] & addr_mask);
    }
    if (ret < 0)
        return ret;

    if (base_pdpt) {
        if (!(base_pdpt[pdpt_idx] & PTE_PRESENT))
            return -PAL_ERROR_INVAL;
        base_pd = memory_frame_pointer(base_pdpt[pdpt_idx] & addr_mask);
    }
    if ((pd[pd_idx] | (base_pd ? base_pd[pd_idx] : 0)) & PTE_HUGE)
        return -PAL_ERROR_INVAL; /* splitting a 2-MiB page is not implemented */
    uint64_t* pt;
    if (base_root && make_private) {
        ret = address_space_get_private_child(&pd[pd_idx], base_pd[pd_idx], space, &pt);
    } else {
        if (!(pd[pd_idx] & PTE_PRESENT))
            return -PAL_ERROR_INVAL;
        pt = memory_frame_pointer(pd[pd_idx] & addr_mask);
    }
    if (ret < 0)
        return ret;

    *out_entry = &pt[pt_idx];
    return 0;
}

__attribute_no_sanitize_address
int memory_address_space_map(struct memory_address_space* space, uintptr_t addr,
                             uintptr_t frame, bool write, bool execute, bool usermode) {
    const uint64_t addr_mask = 0x00000ffffffff000UL;
    if (!space || !space->root || !space->root_phys ||
            addr >= 512UL * 1024 * 1024 * 1024 || !IS_ALIGNED_POW2(addr, PAGE_SIZE) ||
            !IS_ALIGNED_POW2(frame, PAGE_SIZE) || (frame & ~addr_mask))
        return -PAL_ERROR_INVAL;

    uint64_t* pte;
    int ret = address_space_get_leaf_entry(space, addr, /*make_private=*/true, &pte);
    if (ret < 0)
        return ret;

    uint64_t flags = 1UL | (write ? 1UL << 1 : 0) | (usermode ? 1UL << 2 : 0) |
                     (execute ? 0 : 1UL << 63);
    *pte = frame | flags;

    uint64_t current_root;
    __asm__ volatile("mov %%cr3, %0" : "=r"(current_root));
    if ((current_root & addr_mask) == space->root_phys)
        __asm__ volatile("invlpg (%0)" : : "r"(addr) : "memory");
    return 0;
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
    if (!source || !target || !IS_ALIGNED_POW2(addr, PAGE_SIZE) || !size ||
            !IS_ALIGNED_POW2(size, PAGE_SIZE) || addr + size < addr || !out_changed)
        return -PAL_ERROR_INVAL;

    const uint64_t addr_mask = 0x00000ffffffff000UL;
    bool changed = false;
    *out_changed = false;
    for (uintptr_t va = addr; va < addr + size; va += PAGE_SIZE) {
        uint64_t* pte;
        int ret = address_space_get_leaf_entry(source, va, /*make_private=*/false, &pte);
        if (ret < 0)
            return ret;
        uint64_t entry = *pte;
        uint64_t* target_pte;
        ret = address_space_get_leaf_entry(target, va, /*make_private=*/true, &target_pte);
        if (ret < 0)
            return ret;
        if (*target_pte == entry)
            continue;
        *target_pte = entry;
        changed = true;
        uint64_t current_root;
        __asm__ volatile("mov %%cr3, %0" : "=r"(current_root));
        if ((current_root & addr_mask) == target->root_phys)
            __asm__ volatile("invlpg (%0)" : : "r"(va) : "memory");
    }
    *out_changed = changed;
    return 0;
}

int memory_address_space_switch(const struct memory_address_space* space) {
    if (!space || !space->root || !space->root_phys ||
            !IS_ALIGNED_POW2(space->root_phys, PAGE_SIZE))
        return -PAL_ERROR_INVAL;
    return memory_address_space_switch_root(space->root_phys);
}

int memory_address_space_switch_root(uint64_t root) {
    const uint64_t addr_mask = 0x00000ffffffff000UL;
    if (!root || !IS_ALIGNED_POW2(root, PAGE_SIZE) || (root & ~addr_mask))
        return -PAL_ERROR_INVAL;

    uint64_t current_root;
    __asm__ volatile("mov %%cr3, %0" : "=r"(current_root));
    if ((current_root & addr_mask) == root)
        return 0;
    __asm__ volatile("mov %0, %%cr3" : : "r"(root) : "memory");
    return 0;
}
#endif

#define MSR_IA32_PAT 0x277

/* PAT memory types, see Intel SDM, Vol. 3A, Chapter 12. */
#define PAT_TYPE_WC 0x01

static_assert(PAGE_SIZE == 4096, "unexpected PAGE_SIZE (expected 4K)");

/* Graphics GEM arenas (libgraphics maps its buffers here at fixed guest
 * addresses). The data-arena size comes from the manifest key
 * sys.experimental__graphics_data_size, parsed in pal_main and applied by
 * graphics_arenas_set_data_size(); the hugepage arena follows the data arena
 * directly. See also memory_reserve_graphics_arenas(); both must stay in sync
 * with the LibOS (libos_mmap.c) and QEMU (sg-listener.c). The compiled values
 * below are the defaults. */
static void* DATA_REGION                  = (void*)0x100000000ULL;
static size_t DATA_REGION_SIZE            = 1ULL * 1024 * 1024 * 1024;
static void* HUGEPAGE_DATA_REGION         = (void*)0x140000000ULL;
static size_t HUGEPAGE_REGION_SIZE        = 1ULL * 1024 * 1024 * 1024;

void graphics_arenas_set_geometry(uintptr_t data_addr, size_t data_size, uintptr_t huge_addr) {
    assert(data_size);
    /* A zero data address means the VMM supplied no layout: keep the compiled anchor (4G). */
    DATA_REGION          = (void*)(data_addr ? data_addr : 0x100000000ULL);
    DATA_REGION_SIZE     = data_size;
    if (huge_addr) {
        if (huge_addr < (uintptr_t)DATA_REGION + data_size) {
            log_error("Hugepage graphics arena 0x%lx does not follow the data arena "
                      "(end 0x%lx); falling back to computing it",
                      (unsigned long long)huge_addr,
                      (unsigned long long)((uintptr_t)DATA_REGION + data_size));
        } else {
            HUGEPAGE_DATA_REGION = (void*)huge_addr;
        }
    } else {
        /* Layout not supplied by the VMM (older launcher): follow the data arena. */
        HUGEPAGE_DATA_REGION = (void*)((uintptr_t)DATA_REGION + data_size);
    }
    log_always("Graphics arenas: data at %p (%lu MiB), hugepage at %p (%lu MiB)",
               DATA_REGION, (unsigned long)(DATA_REGION_SIZE / 1024 / 1024),
               HUGEPAGE_DATA_REGION, (unsigned long)(HUGEPAGE_REGION_SIZE / 1024 / 1024));
}

int memory_reserve_graphics_arenas(
        int (*callback)(uintptr_t addr, size_t size, bool is_user_reserved,
                        const char* comment)) {
    /* Graphics is a VM feature; a guest whose RAM does not reach the arena
     * window (possible for a non-graphics guest started at a small memory size)
     * simply gets no reservation -- its allocator could not use that physical
     * range anyway. Graphics guests that configure an arena past the end of RAM
     * are stopped here too, and the launcher enforces the RAM floor up front. */
    if ((uintptr_t)HUGEPAGE_DATA_REGION + HUGEPAGE_REGION_SIZE >
            (uintptr_t)g_pal_public_state.memory_address_end) {
        log_always("Graphics arenas at %p (%lu B + hugepage) fall outside guest RAM "
                   "(end %p); not reserved",
                   DATA_REGION, (unsigned long)DATA_REGION_SIZE,
                   g_pal_public_state.memory_address_end);
        return 0;
    }

    int ret = callback((uintptr_t)DATA_REGION, DATA_REGION_SIZE,
                       /*is_user_reserved=*/true, "egl_redirect_data");
    if (ret < 0)
        return -PAL_ERROR_NOMEM;
    ret = callback((uintptr_t)HUGEPAGE_DATA_REGION, HUGEPAGE_REGION_SIZE,
                   /*is_user_reserved=*/true, "egl_redirect_huge");
    if (ret < 0)
        return -PAL_ERROR_NOMEM;
    return 0;
}

static inline bool is_egl_redirect_data_region_base(void* addr, size_t size) {
    if (addr == DATA_REGION || addr == HUGEPAGE_DATA_REGION) {
        size_t expected_size = addr == DATA_REGION ? DATA_REGION_SIZE : HUGEPAGE_REGION_SIZE;
        /* In our setup, these bases are reserved exclusively for libgraphics regions and must be
         * mapped as a single fixed-size chunk. Fail loudly if assumptions are violated. */
        if (size != expected_size) {
            log_error("Unexpected mapping size at special base %p: got %lu, expected %lu",
                      addr, (unsigned long)size, (unsigned long)expected_size);
            BUG();
        }
        if (!IS_ALIGNED_POW2((uintptr_t)addr, PAGE_SIZE) || !IS_ALIGNED_POW2(size, PAGE_SIZE)) {
            log_error("Unexpected alignment for special base %p (size %lu)", addr,
                      (unsigned long)size);
            BUG();
        }
        return true;
    }
    return false;
}

static inline bool is_egl_redirect_data_region_range(void* addr, size_t size) {
    uintptr_t start = (uintptr_t)addr;
    uintptr_t end;
    if (!size || __builtin_add_overflow(start, size, &end))
        return false;

    uintptr_t data_start = (uintptr_t)DATA_REGION;
    uintptr_t data_end = data_start + DATA_REGION_SIZE;
    uintptr_t huge_start = (uintptr_t)HUGEPAGE_DATA_REGION;
    uintptr_t huge_end = huge_start + HUGEPAGE_REGION_SIZE;

    return (data_start <= start && end <= data_end) ||
           (huge_start <= start && end <= huge_end);
}

/* Beginning of the page table hierarchy */
uint64_t g_pml4_table_base = 0;

#ifdef __x86_64__
struct memory_address_space* memory_address_space_boot(void) {
    return &g_boot_address_space;
}
#endif

/* Address Sanitizer shadow memory (physical memory range) */
static uint64_t g_asan_shadow_phys_start = 0;
static uint64_t g_asan_shadow_phys_end   = 0;

/* Global memory address range */
static uintptr_t g_memory_address_start = 0;
static uintptr_t g_memory_address_end   = 0;

void* memory_get_shared_region(size_t size) {
	/* trivial shared memory management: allocations in the shared-memory range
	 * [SHARED_MEM_ADDR, SHARED_MEM_ADDR + SHARED_MEM_SIZE) are ever increasing */
	static void* g_shared_heap_pos = (void*)SHARED_MEM_ADDR;

    if (!size)
        return NULL;

#ifdef ASAN
    size_t original_size = size;
#endif
    size = ALIGN_UP(size, PAGE_SIZE);

    assert((uintptr_t)g_shared_heap_pos >= SHARED_MEM_ADDR);
    assert(access_ok(g_shared_heap_pos, size));
    if ((uintptr_t)g_shared_heap_pos + size > SHARED_MEM_ADDR + SHARED_MEM_SIZE)
        return NULL;

    void* ret = g_shared_heap_pos;
    g_shared_heap_pos = (char*)g_shared_heap_pos + size;
#ifdef ASAN
    asan_unpoison_region((uintptr_t)ret, original_size);
#endif
    return ret;
}

int memory_free_shared_region(void* addr, size_t size) {
    /* dummy; we never free shared regions for simplicity */
    __UNUSED(addr);
    __UNUSED(size);
#ifdef ASAN
    asan_poison_region((uintptr_t)addr, size, ASAN_POISON_USER);
#endif
    return 0;
}

__attribute_no_sanitize_address
int memory_find_page_table_entry(uint64_t addr, uint64_t** out_pte_addr) {
    assert(g_pml4_table_base);
    uint64_t* pml4_table = memory_frame_pointer(g_pml4_table_base);

    /* the mask covers bits 12:43 which covers the address space [0, 16TB); note that we do not
     * cover up to 12:51 because bit 47 or 51 can be specially used (e.g. "shared" bit in TDX) */
    const uint64_t page_table_entry_addr_mask = 0x00000ffffffff000UL;

    /* there is a single entry in the PML4 table, see also memory_pagetables_init();
     * in this entry, bits 12:51 contain the address of the PDPT table */
    uint64_t* pdpt_table = memory_frame_pointer(pml4_table[0] & page_table_entry_addr_mask);

    /* each PDPT table entry covers 1GB of memory, starting from addr 0x00 */
    size_t pdpt_table_idx = addr / 1024 / 1024 / 1024;
    uint64_t* pd_table = memory_frame_pointer(pdpt_table[pdpt_table_idx] &
                                              page_table_entry_addr_mask);

    /* each PD table entry covers 2MB of memory in the 1GB memory region determined via PDPT table
     * entry (recall that there are 512 PD entries in one PD table) */
    size_t pd_table_idx = (addr / 1024 / 1024 / 2) % 512;
    uint64_t* pt_table = memory_frame_pointer(pd_table[pd_table_idx] &
                                              page_table_entry_addr_mask);

    /* each PT table entry covers 4KB of memory in the 2MB memory region determined via PD table
     * entry (recall that there are 512 PD entries in one PT table) */
    size_t pt_table_idx = (addr / 1024 / 4) % 512;

    /* sanity check: must arrive at the same page address as in `addr` */
    uint64_t page_addr = pt_table[pt_table_idx] & page_table_entry_addr_mask;
    if ((addr & page_table_entry_addr_mask) != page_addr)
        return -PAL_ERROR_INVAL;

    *out_pte_addr = &pt_table[pt_table_idx];
    return 0;
}

__attribute_no_sanitize_address
int memory_mark_pages_off(uint64_t addr, size_t size) {
    for (uint64_t mark_addr = addr; mark_addr < addr + size; mark_addr += PAGE_SIZE) {
        uint64_t* pte_addr;
        int ret = memory_find_page_table_entry(mark_addr, &pte_addr);
        if (ret < 0)
            return ret;
        *pte_addr &= ~1UL;
    }
    return send_invalidate_tlb_ipi_and_wait((void*)addr, size, /*invalidate_on_this_cpu=*/true);
}

__attribute_no_sanitize_address
int memory_mark_pages_on(uint64_t addr, size_t size, bool write, bool execute, bool usermode) {
    for (uint64_t mark_addr = addr; mark_addr < addr + size; mark_addr += PAGE_SIZE) {
        uint64_t* pte_addr;
        int ret = memory_find_page_table_entry(mark_addr, &pte_addr);
        if (ret < 0)
            return ret;

        uint64_t bits = 1UL; /* present bit is always set, since page is at least readable */
        if (write)
            bits |= 1UL << 1;
        if (usermode)
            bits |= 1UL << 2;
        if (!execute)
            bits |= 1UL << 63; /* NX/XD bit */
        *pte_addr = (*pte_addr & ~((1UL << 63) + 7UL)) | bits;
    }
    return send_invalidate_tlb_ipi_and_wait((void*)addr, size, /*invalidate_on_this_cpu=*/true);
}

__attribute_no_sanitize_address
int memory_mark_pages_strong_uncacheable(uint64_t addr, size_t size, bool mark) {
    for (uint64_t mark_addr = addr; mark_addr < addr + size; mark_addr += PAGE_SIZE) {
        uint64_t* pte_addr;
        int ret = memory_find_page_table_entry(mark_addr, &pte_addr);
        if (ret < 0)
            return ret;

        if (mark)
            *pte_addr |= 1UL << 4; /* PCD = Page-level cache disable */
        else
            *pte_addr &= ~(1UL << 4);
    }
    return send_invalidate_tlb_ipi_and_wait((void*)addr, size, /*invalidate_on_this_cpu=*/true);
}

__attribute_no_sanitize_address
int memory_mark_pages_write_combined(uint64_t addr, size_t size, bool mark) {
    /* Use PAT index 1 for WC, i.e. PWT=1, PCD=0, PAT=0 for 4KB pages. */
    const uint64_t pwt_bit = 1UL << 3;
    const uint64_t pcd_bit = 1UL << 4;
    const uint64_t pat_bit = 1UL << 7;

    for (uint64_t mark_addr = addr; mark_addr < addr + size; mark_addr += PAGE_SIZE) {
        uint64_t* pte_addr;
        int ret = memory_find_page_table_entry(mark_addr, &pte_addr);
        if (ret < 0)
            return ret;

        uint64_t pte = *pte_addr;
        pte &= ~(pwt_bit | pcd_bit | pat_bit);
        if (mark) {
            pte |= pwt_bit;
        }
        *pte_addr = pte;
    }
    return send_invalidate_tlb_ipi_and_wait((void*)addr, size, /*invalidate_on_this_cpu=*/true);
}

void memory_pat_init_for_wc(void) {
    /* PAT is per-CPU state; may be called on each CPU. */
    unsigned int words[CPUID_WORD_NUM] = {0};
    cpuid(FEATURE_FLAGS_LEAF, /*subleaf=*/0, words);
    if (!(words[CPUID_WORD_EDX] & (1U << 16))) { /* PAT feature bit */
        log_error("CPU does not report PAT support; cannot configure WC");
        BUG();
    }

    uint64_t pat = rdmsr(MSR_IA32_PAT);

    if (((pat >> 8) & 0xffU) == PAT_TYPE_WC) {
        /* Already configured on this CPU. */
        return;
    }

    /* Program PAT entry #1 (bits 8..15) to WC. Keep all other entries intact. */
    uint64_t new_pat = (pat & ~(0xffUL << 8)) | ((uint64_t)PAT_TYPE_WC << 8);
    wrmsr(MSR_IA32_PAT, new_pat);

    uint64_t verify_pat = rdmsr(MSR_IA32_PAT);
    if (verify_pat != new_pat) {
        log_error("Failed to program IA32_PAT for WC (old=0x%lx new=0x%lx readback=0x%lx)",
                  pat, new_pat, verify_pat);
        BUG();
    }

    log_always("IA32_PAT configured: old=0x%lx new=0x%lx (WC at index 1)", pat, new_pat);
}

/*
 * For Address Sanitizer, we reserve 1/8th of the total VM address space (which includes the memory
 * and PCI holes at [2GB, 4GB)) to ASan shadow memory. We carve out this 1/8th of the space at the
 * very end of the usable VM address space:
 *   - If VM has 8GB of RAM, then ASan needs 1GB, i.e. [7G, 8G).
 *   - Current max amount of RAM for VM is 64GB, which roughly corresponds to 8GB of shadow memory.
 *
 * We force VM size to be at least 8GB for simplicity: to avoid overlapping with reserved and
 * MMIO memory regions (e.g., shared memory region, page tables region, PCI MMIO region). Otherwise
 * for example, if VM size would be 4GB, then ASan shadow memory could not be put at [3.5GB, 4GB) as
 * it overlaps with PCI MMIO region.
 *
 * In addition, this ASan shadow memory must be reflected in ASan-specific page tables. These page
 * tables do not have 1:1 mapping, as ASan shadow memory has virtual addresses [1.5TB, 1.5TB + 8GB).
 * Thus, these page tables must create virtual-to-physical mapping as:
 *
 *     1.5TB + offset -> ASAN_SHADOW_PHYSICAL_START + offset
 *
 * Also, the "normal" page tables must disable "normal" virtual mappings to these physical-shadow
 * memory pages (otherwise normal memory would alias ASan shadow memory).
 */
__attribute_no_sanitize_address
static int asan_init(void* memory_address_end, uint64_t page_tables_addr, size_t page_tables_size,
                     uint64_t pml4_table_base, size_t normal_tables_cnt) {
#ifdef ASAN
    assert((uint64_t)memory_address_end <= ASAN_SHADOW_START);

    uint64_t vm_address_space_size = (uint64_t)memory_address_end;

    /* with ASan, Gramine requires at least 8GB of VM space; not that we use "7" in the expression
     * below to take into account situations when the VMM hides a bit of space at the end of 8GB for
     * its own purposes (this happens in TDX case) */
    if (vm_address_space_size < 7UL * 1024 * 1024 * 1024) {
        /* FIXME: print error via Port I/O because normal log fails at this early boot stage */
        debug_serial_io_write("Failed to initialize Address Sanitizer: VM size is less than 8GB");
        return -PAL_ERROR_NOMEM;
    }

    uint64_t asan_shadow_phys_start = (uint64_t)memory_address_end - vm_address_space_size / 8;
    asan_shadow_phys_start = ALIGN_DOWN(asan_shadow_phys_start, PAGE_SIZE);

    uint64_t asan_shadow_phys_end = asan_shadow_phys_start + vm_address_space_size / 8;
    asan_shadow_phys_end = ALIGN_DOWN(asan_shadow_phys_end, PAGE_SIZE);

    uint64_t asan_shadow_size = asan_shadow_phys_end - asan_shadow_phys_start;
    assert(asan_shadow_phys_start >= SHARED_MEM_ADDR + SHARED_MEM_SIZE);
    assert(asan_shadow_phys_end <= (uint64_t)memory_address_end);
    assert(asan_shadow_size <= vm_address_space_size / 8);
    assert(!(asan_shadow_phys_start < 0x100000000UL && 0x80000000UL < asan_shadow_phys_end));

    /* reset ASan shadow memory for sanity; we rely on 1:1 mapping set up by pagetables_init() */
    _real_memset((void*)asan_shadow_phys_start, 0, asan_shadow_size);

    size_t pages_4k_cnt = UDIV_ROUND_UP(asan_shadow_size, 4UL * 1024);
    size_t pages_2m_cnt = UDIV_ROUND_UP(asan_shadow_size, 2UL * 1024 * 1024);
    size_t pages_1g_cnt = UDIV_ROUND_UP(asan_shadow_size, 1UL * 1024 * 1024 * 1024);

    size_t page_tables_cnt     = UDIV_ROUND_UP(pages_4k_cnt, 512);
    size_t page_dir_tables_cnt = UDIV_ROUND_UP(pages_2m_cnt, 512);

    assert(pages_4k_cnt && pages_2m_cnt && pages_1g_cnt && page_tables_cnt && page_dir_tables_cnt);

    /* we reuse the same PML4 page that is used for normal page tables */
    size_t asan_tables_cnt = page_tables_cnt + page_dir_tables_cnt + /*PDP=*/1 + /*PML4=*/0;
    if ((normal_tables_cnt + asan_tables_cnt) * PAGE_SIZE > page_tables_size) {
        /* normal + asan page tables can occupy no more than page_tables_size, check for sanity */
        return -PAL_ERROR_NOMEM;
    }

    /* ASan memory is accessible only from kernel mode (ring 0), RW, present, non-executable */
    uint64_t flags = 0x3 + (1UL << 63);

    /* page tables for ASan shadow memory start after PML4 table (ASan PTs follow normal PTs) */
    uint64_t ptr = pml4_table_base;
    assert(IS_ALIGNED(ptr, PAGE_SIZE));
    ptr += PAGE_SIZE;

    /* page tables with PTE leaf entries */
    uint64_t page_tables_base = ptr;
    uint64_t asan_shadow_phys_addr = asan_shadow_phys_start;
    for (size_t i = 0; i < pages_4k_cnt; i++) {
        uint64_t entry = asan_shadow_phys_addr + flags;
        _real_memcpy((void*)ptr, &entry, sizeof(entry));
        ptr += sizeof(entry);
        asan_shadow_phys_addr += PAGE_SIZE;
    }
    ptr = ALIGN_UP(ptr, PAGE_SIZE);

    /* page directory tables with PDE entries */
    uint64_t page_dir_tables_base = ptr;
    for (size_t i = 0; i < pages_2m_cnt; i++) {
        uint64_t entry = page_tables_base + ((ptr - page_dir_tables_base) / 8) * PAGE_SIZE + flags;
        _real_memcpy((void*)ptr, &entry, sizeof(entry));
        ptr += sizeof(entry);
    }
    ptr = ALIGN_UP(ptr, PAGE_SIZE);

    /* one PDP page with up to 8 PDPE entries (to cover up to 8GB of shadow memory) */
    uint64_t pdp_table_base = ptr;
    for (size_t i = 0; i < pages_1g_cnt; i++) {
        uint64_t entry = page_dir_tables_base + ((ptr - pdp_table_base) / 8) * PAGE_SIZE + flags;
        _real_memcpy((void*)ptr, &entry, sizeof(entry));
        ptr += sizeof(entry);
    }
    uint64_t first_unused_page_in_page_tables = ALIGN_UP(ptr, PAGE_SIZE);

    /* update the "normal" PML4 page with an additional PML4E entry */
    assert(ASAN_SHADOW_START % (512UL * 1024 * 1024 * 1024) == 0);
    uint64_t entry_in_pml4_idx = ASAN_SHADOW_START / (512UL * 1024 * 1024 * 1024);
    assert(entry_in_pml4_idx != 0);

    ptr = pml4_table_base;
    for (size_t i = 0; i < entry_in_pml4_idx; i++)
        ptr += sizeof(uint64_t);

    uint64_t entry = (uint64_t)pdp_table_base + flags;
    _real_memcpy((void*)ptr, &entry, sizeof(entry));

    __asm__ volatile("mov %%rax, %%cr3" : : "a"(pml4_table_base));

    /* explicitly disallow accesses to the whole shared memory region to catch bugs in shared memory
     * (e.g. in virtio drivers or in the TDX Quote protocol); see also memory_get_shared_region()
     * and memory_free_shared_region() */
    asan_poison_region(SHARED_MEM_ADDR, SHARED_MEM_SIZE, ASAN_POISON_USER);

    size_t unused_pages_in_page_tables_size = page_tables_addr + page_tables_size
                                                  - first_unused_page_in_page_tables;
    asan_poison_region(first_unused_page_in_page_tables, unused_pages_in_page_tables_size,
                       ASAN_POISON_USER);

    g_asan_shadow_phys_start = asan_shadow_phys_start;
    g_asan_shadow_phys_end   = asan_shadow_phys_end;
#else
    __UNUSED(memory_address_end);
    __UNUSED(page_tables_addr);
    __UNUSED(page_tables_size);
    __UNUSED(pml4_table_base);
    __UNUSED(normal_tables_cnt);
#endif
    return 0;
}

/* sets up the new page tables hierarchy (with 4KB pages) to cover memory range [0x0, memory_size);
 * page tables have 1:1 virtual-to-physical address translation */
__attribute_no_sanitize_address
static int pagetables_init(size_t memory_size, uint64_t page_tables_addr, size_t page_tables_size,
                           size_t* out_total_tables_cnt, uint64_t* out_pml4_table_base) {
    /* all memory is initially accessible only from kernel mode (ring 0), RW and present */
    uint64_t leaf_flags = 0x3;

    log_always("Initializing page tables for memory size %zu MB at addr 0x%lx",
               memory_size / (1024 * 1024), page_tables_addr);

    /* for MMU traversal in user mode, mid-tree entries must be accessible also in ring-3 */
    uint64_t tree_flags = 0x7;

    size_t pages_4k_cnt = UDIV_ROUND_UP(memory_size, 4 * 1024);
    size_t pages_2m_cnt = UDIV_ROUND_UP(memory_size, 2 * 1024 * 1024);
    size_t pages_1g_cnt = UDIV_ROUND_UP(memory_size, 1 * 1024 * 1024 * 1024);

    size_t page_tables_cnt     = UDIV_ROUND_UP(pages_4k_cnt, 512);
    size_t page_dir_tables_cnt = UDIV_ROUND_UP(pages_2m_cnt, 512);

    assert(pages_4k_cnt && pages_2m_cnt && pages_1g_cnt && page_tables_cnt && page_dir_tables_cnt);

    assert(pages_4k_cnt && pages_2m_cnt && pages_1g_cnt && page_tables_cnt && page_dir_tables_cnt);

    size_t total_tables_cnt = page_tables_cnt + page_dir_tables_cnt + /*PDP=*/1 + /*PML4=*/1;
    if (total_tables_cnt * PAGE_SIZE > page_tables_size) {
        /* page tables can occupy no more than page_tables_size, check for sanity */
        return -PAL_ERROR_NOMEM;
    }

    uint64_t ptr = page_tables_addr;

    /* page tables with PTE leaf entries */
    uint64_t page_tables_base = ptr;
    for (size_t i = 0; i < pages_4k_cnt; i++) {
        uint64_t entry = ((ptr - page_tables_base) / 8) * PAGE_SIZE + leaf_flags;
        _real_memcpy((void*)ptr, &entry, sizeof(entry));
        ptr += sizeof(entry);
    }

    if (!IS_ALIGNED(ptr, PAGE_SIZE))
        return -PAL_ERROR_INVAL;

    /* page directory tables with PDE entries */
    uint64_t page_dir_tables_base = ptr;
    for (size_t i = 0; i < pages_2m_cnt; i++) {
        uint64_t entry = page_tables_base + ((ptr - page_dir_tables_base) / 8) * PAGE_SIZE
                                          + tree_flags;
        _real_memcpy((void*)ptr, &entry, sizeof(entry));
        ptr += sizeof(entry);
    }

    if (!IS_ALIGNED(ptr, PAGE_SIZE))
        return -PAL_ERROR_INVAL;

    /* one PDP page with up to 512 PDPE entries */
    uint64_t pdp_table_base = ptr;
    for (size_t i = 0; i < pages_1g_cnt; i++) {
        uint64_t entry = page_dir_tables_base + ((ptr - pdp_table_base) / 8) * PAGE_SIZE
                                              + tree_flags;
        _real_memcpy((void*)ptr, &entry, sizeof(entry));
        ptr += sizeof(entry);
    }
    ptr = ALIGN_UP(ptr, PAGE_SIZE);

    /* one PML4 page with a single PML4E entry */
    uint64_t pml4_table_base = ptr;
    uint64_t entry = (uint64_t)pdp_table_base + tree_flags;
    _real_memcpy((void*)ptr, &entry, sizeof(entry));

    __asm__ volatile("mov %%rax, %%cr3" : : "a"(pml4_table_base));

    if (out_total_tables_cnt)
        *out_total_tables_cnt = total_tables_cnt;
    if (out_pml4_table_base)
        *out_pml4_table_base = pml4_table_base;
    return 0;
}

__attribute_no_sanitize_address
int memory_pagetables_init(void* memory_address_end, bool current_page_tables_cover_1gb) {
    int ret;

    /*
     * First set up temporary page tables hierarchy to cover range [0x0, 1GB), if current page
     * tables don't do this already. To cover this range, it's enough to have page tables in the
     * region [4MB, 8MB) -- these temporary page tables will be overwritten anyway by the actual
     * page tables hierarchy set up below.
     *
     * Note that at this point only the PAL binary is loaded into memory, and it is either located
     * at [1MB, 4MB) in non-TDX case, or somewhere at 1GB or higher in TDX case. So there will be no
     * overlap with this temporary page tables hierarchy at [4MB, 8MB).
     *
     * We need this temporary hierarchy because bootloaders of some PALs (e.g. VM) set up static
     * page tables that cover small range [0x0, 32MB) which doesn't contain the [PAGE_TABLES_ADDR,
     * PAGE_TABLES_ADDR + PAGE_TABLES_SIZE) final page-tables region.
     */
    if (!current_page_tables_cover_1gb) {
        ret = pagetables_init(/*memory_size=*/1UL * 1024 * 1024 * 1024,
                              /*page_tables_addr=*/4UL * 1024 * 1024,
                              /*page_tables_size=*/4UL * 1024 * 1024,
                              /*out_total_tables_cnt=*/NULL, /*out_pml4_table_base=*/NULL);
        if (ret < 0)
            return ret;
    }

    /* now set up the new page tables hierarchy to cover range [0x0, memory_address_end) (aligned to
     * the next power of 2); we will use these page tables until the end of execution */
    size_t memory_size = 1;
    while (memory_size < (uintptr_t)memory_address_end)
        memory_size *= 2;

    /* memory size must be at least 4GB in size, because [3GB, 4GB) range is used for PCI, etc. */
    if (memory_size < 4UL * 1024 * 1024 * 1024)
        memory_size = 4UL * 1024 * 1024 * 1024;

 
    log_always("Memory size for page tables: %zu GB\n", memory_size / (1024 * 1024 * 1024));

    /* memory size must be at most 512GB in size, because the current construction of page tables
     * relies on a single PDP page which limits max addressable memory to 512GB */
    if (memory_size > 512UL * 1024 * 1024 * 1024)
        return -PAL_ERROR_OVERFLOW;

    size_t total_tables_cnt;
    uint64_t pml4_table_base;
    ret = pagetables_init(memory_size, PAGE_TABLES_ADDR, PAGE_TABLES_SIZE, &total_tables_cnt,
                          &pml4_table_base);
    if (ret < 0)
        return ret;

    ret = asan_init(memory_address_end, PAGE_TABLES_ADDR, PAGE_TABLES_SIZE, pml4_table_base,
                    total_tables_cnt);
    if (ret < 0)
        return ret;

    g_pml4_table_base = pml4_table_base;
#ifdef __x86_64__
    g_boot_address_space.root = (uint64_t*)pml4_table_base;
    g_boot_address_space.root_phys = pml4_table_base;
    g_boot_address_space.base_root = (uint64_t*)pml4_table_base;
#endif
    return 0;
}

int memory_preload_ranges(e820_table_entry* e820_entries, size_t e820_entries_size,
                          int (*callback)(uintptr_t addr, size_t size, bool is_user_reserved,
                                          const char* comment)) {
    int ret;

    for (size_t i = 0; i < e820_entries_size / sizeof(*e820_entries); i++) {
        if (e820_entries[i].type == E820_ADDRESS_RANGE_MEMORY)
            continue;

        if (PCI_HOLE_ADDR <= e820_entries[i].address &&
                e820_entries[i].address + e820_entries[i].size <= PCI_HOLE_ADDR + PCI_HOLE_SIZE) {
            /* reported reserved memory region is fully consumed by the PCI hole region */
            continue;
        }

        if (e820_entries[i].address < PAGE_TABLES_ADDR + PAGE_TABLES_SIZE &&
                PAGE_TABLES_ADDR < e820_entries[i].address + e820_entries[i].size) {
            /* a reserved range overlaps with our page tables range */
            return -PAL_ERROR_DENIED;
        }

        if (e820_entries[i].address < SHARED_MEM_ADDR + SHARED_MEM_SIZE &&
                SHARED_MEM_ADDR < e820_entries[i].address + e820_entries[i].size) {
            /* a reserved range overlaps with our shared memory range */
            return -PAL_ERROR_DENIED;
        }

        ret = callback(e820_entries[i].address, e820_entries[i].size,
                       /*is_user_reserved=*/false, "E820 reserved");
        if (ret < 0)
            return -PAL_ERROR_NOMEM;
    }

    /*
     * Mark the following memory regions as reserved (in addition to the above ones, extracted from
     * the E820 table), so that memory allocator doesn't use them:
     *   - [4KB, 1MB):     legacy DOS (includes DOS area, SMM memory, System BIOS),
     *   - [512MB, 658MB): page tables (for app memory and ASan shadow memory),
     *   - [658MB, 896MB): shared memory for virtqueues and for Quote (in TDX case),
     *   - [2GB, 3GB):     memory hole (QEMU doesn't map any memory here),
     *   - [3GB, 4GB):     PCI (includes BARs, LAPIC, IOAPIC).
     *   - [VM_RAM_END - 1/8th VM_RAM, VM_RAM_END):
     *                     Address Sanitizer shadow memory (physical memory region).
     *
     * Some notes on how this layout interplays with other regions:
     *   - Region [0, 4KB) is not marked as reserved (but page protections are NONE, see below).
     *     This is required to allow memory faults on special address 0x0; if we would mark this
     *     region as reserved in LibOS, it would die with "Internal memory fault with VMA" on 0x0.
     *   - In non-TDX case, the PAL binary is put at [1MB, 4MB).
     *   - In TDX case, the PAL binary is put at the top of RAM (but below 2GB). We enforce RAM to
     *     be at least 1GB, so in the worst case, the PAL binary + other TDShim data is put at
     *     [896MB, 1024MB). TDShim uses up to 128MB for the payload (PAL binary) and other data.
     *   - Applications may be EXEC programs, which means they will be put at 4MB. The largest app
     *     binaries we've seen in the wild have up to 300MB. Our layout allows to put such app
     *     binary at [4MB, 512MB), which is enough to host a 508MB-sized binary.
     */
    ret = callback(0x1000UL, 0x100000UL - 0x1000UL, /*is_user_reserved=*/false,
                   "dos_memory_addr");
    if (ret < 0)
        return -PAL_ERROR_NOMEM;
    ret = callback(PAGE_TABLES_ADDR, PAGE_TABLES_SIZE, /*is_user_reserved=*/false, "page_tables");
    if (ret < 0)
        return -PAL_ERROR_NOMEM;
    ret = callback(SHARED_MEM_ADDR, SHARED_MEM_SIZE, /*is_user_reserved=*/false, "shared_memory");
    if (ret < 0)
        return -PAL_ERROR_NOMEM;
    ret = callback(PCI_HOLE_ADDR, PCI_HOLE_SIZE, /*is_user_reserved=*/false, "qemu_pci_hole");
    if (ret < 0)
        return -PAL_ERROR_NOMEM;

    if (g_asan_shadow_phys_start && g_asan_shadow_phys_end) {
        ret = callback(g_asan_shadow_phys_start, g_asan_shadow_phys_end - g_asan_shadow_phys_start,
                       /*is_user_reserved=*/false, "asan_shadow_memory");
        if (ret < 0)
            return -PAL_ERROR_DENIED;
    }

    /* The libgraphics GEM arenas are reserved separately in
     * memory_reserve_graphics_arenas(), after the manifest has been parsed in
     * pal_main (their geometry depends on the manifest) -- not here, where the
     * manifest is not yet available. */

    return 0;
}

/* a counterpart to memory_preload_ranges(), must be executed after all PAL VM initialization is
 * done; we tighten page permissions on PAL-kernel specific memory regions, see the list in
 * memory_preload_ranges() above */
__attribute_no_sanitize_address
int memory_tighten_permissions(void) {
    int ret;

    /*
     * [0, 1MB): Legacy DOS (includes DOS area, SMM memory, System BIOS). We could not disable these
     *           pages at PAL init, because a sub-region was used for AP multicore init code/stack.
     */
    ret = memory_mark_pages_off(0x0UL, 0x100000UL);
    if (ret < 0)
        return ret;
    /*
     * [512MB, 658MB): Page tables. Should be RW (to allow page-permission changes), but accessible
     *                 only from ring-0 (no usermode) and cannot be executed.
     */
    ret = memory_mark_pages_on(PAGE_TABLES_ADDR, PAGE_TABLES_SIZE, /*write=*/true,
                               /*execute=*/false, /*usermode=*/false);
    if (ret < 0)
        return ret;
    /*
     * [658MB, 896MB): Shared memory for virtqueues and for Quote (in TDX case). Same as PTs.
     */
    ret = memory_mark_pages_on(SHARED_MEM_ADDR, SHARED_MEM_SIZE, /*write=*/true,
                               /*execute=*/false, /*usermode=*/false);
    if (ret < 0)
        return ret;
    /*
     * [2GB, 3GB): Memory hole (QEMU doesn't map any memory here),
     * [3GB, 4GB): PCI (includes BARs, LAPIC, IOAPIC).
     *
     * NOTE: We would need to perform the following logic: disable the whole region, with exceptions
     * of virtio drivers' (1) interrupt status reg, and (2) RQ/TQ notify addresses. This logic is
     * complicated, and environments like Intel TDX do not allow direct MMIO access anyway, so we
     * don't disable this hole/PCI region as it doesn't affect security.
     */

    ret = memory_mark_pages_on(0xC0000000UL, 0x10000000UL, /*write=*/true,
                               /*execute=*/false, /*usermode=*/true);
    if (ret < 0) {
        log_always("Failed to mark pages 0xC0000000-0xD0000000 as RW and Usermode\n");
        return ret;
    }

    ret = memory_mark_pages_on(0x100000000UL, 0x10000000UL, /*write=*/true,
                               /*execute=*/false, /*usermode=*/true);
    if (ret < 0) {
        log_always("Failed to mark pages 0x0x100000000-0x200000000 as RW and Usermode\n");
        return ret;
    }

    // Set these pages as uncacheable
    ret = memory_mark_pages_strong_uncacheable(0xC0000000UL, 0x10000000UL, /*mark=*/true);
    if (ret < 0) {
        log_always("Failed to mark pages 0xC0000000-0xD0000000 as uncacheable\n");
        return ret;
    } 

#if 0
    log_always("Marking data pages as uncacheable");
    ret = memory_mark_pages_strong_uncacheable(DATA_REGION, DATA_SIZE, /*mark=*/true);
    if (ret < 0) {
        log_always("Failed to mark pages as uncacheable");
        return ret;
    }

    log_always("Marking hugepage data pages as uncacheable");
    ret = memory_mark_pages_strong_uncacheable(HUGEPAGE_DATA_REGION, DATA_SIZE, /*mark=*/true);
    if (ret < 0) {
        log_always("Failed to mark pages as uncacheable");
        return ret;
    }
#endif 

    /*
     * "Normal" page tables must disable "normal" virtual mappings to the physical-shadow memory
     * pages reserved for Address Sanitizer (otherwise normal memory would alias ASan shadow
     * memory, leading to both the app/Gramine and ASan using the same memory).
     */
    if (g_asan_shadow_phys_start && g_asan_shadow_phys_end) {
        ret = memory_mark_pages_off(g_asan_shadow_phys_start,
                                    g_asan_shadow_phys_end - g_asan_shadow_phys_start);
        if (ret < 0)
            return -PAL_ERROR_DENIED;
    }

    // TODO: check why this returns incorrect values..
    log_always("(PAL) Physical Memory Range (%p -- %p)", g_memory_address_start, g_memory_address_end);

    return 0;
}

__attribute_no_sanitize_address
int memory_init(e820_table_entry* e820_entries, size_t e820_entries_size,
                void** out_memory_address_start, void** out_memory_address_end) {
    assert(e820_entries_size % sizeof(*e820_entries) == 0);

    uint64_t memory_address_start = UINT64_MAX;
    uint64_t memory_address_end   = 0;
    for (size_t i = 0; i < e820_entries_size / sizeof(*e820_entries); i++) {
        if (e820_entries[i].type != E820_ADDRESS_RANGE_MEMORY)
            continue;

        // if (memory_address_start > e820_entries[i].address)
        //     memory_address_start = e820_entries[i].address;

        // if (memory_address_end < e820_entries[i].address + e820_entries[i].size)
        //     memory_address_end = e820_entries[i].address + e820_entries[i].size;

        uint64_t addr = e820_entries[i].address;
        uint64_t sz   = e820_entries[i].size;

        /* ignore empty regions */
        if (sz == 0)
            continue;

        /* detect overflow on addr + sz (we keep end as exclusive) */
        if (addr > UINT64_MAX - sz) {
            /* handle as error or clamp to UINT64_MAX */
            memory_address_end = UINT64_MAX;
        } else {
            uint64_t end = addr + sz; /* exclusive end */
            if (memory_address_end < end)
                memory_address_end = end;
        }

        if (memory_address_start > addr)
            memory_address_start = addr;

    }

    /* never start at 0x0, see explanation in memory_preload_ranges() */
    if (memory_address_start == 0x0)
        memory_address_start = 0x1000;

    if (memory_address_start >= memory_address_end)
        return -PAL_ERROR_DENIED;

    if (memory_address_start > PAGE_TABLES_ADDR ||
            memory_address_end < PAGE_TABLES_ADDR + PAGE_TABLES_SIZE)
        return -PAL_ERROR_DENIED;

    if (memory_address_start > SHARED_MEM_ADDR ||
            memory_address_end < SHARED_MEM_ADDR + SHARED_MEM_SIZE)
        return -PAL_ERROR_DENIED;

    *out_memory_address_start = (void*)memory_address_start;
    *out_memory_address_end   = (void*)memory_address_end;

    // VERY BAD: hardcode memory end to 10GiB
    // *out_memory_address_end   = (void*) (10*1024*1024*1024);

    g_memory_address_start = (void*)memory_address_start;
    g_memory_address_end   = (void*)memory_address_end;
    return 0;
}

int memory_alloc(void* addr, size_t size, bool read, bool write, bool execute) {
    if ((uintptr_t)addr < SHARED_MEM_ADDR + SHARED_MEM_SIZE &&
            SHARED_MEM_ADDR < (uintptr_t)addr + size) {
        /* [addr, addr+size) at least partially overlaps shared memory, should be impossible */
        return -PAL_ERROR_DENIED;
    }

    if (!read && !write && !execute) {
        memory_mark_pages_off((uint64_t)addr, size);
#ifdef ASAN
        asan_poison_region((uintptr_t)addr, size, ASAN_POISON_USER);
#endif
        return 0;
    }

    /* we rely on CR0.WP == 0 (Write Protect disabled), which allows to write even into read-only
     * pages in ring 0 (otherwise for read-only allocs, we would need to call below function twice:
     * once with W permission, and after memset-to-zero again, without W permission) */
    // log_always("Marking pages on: %p -- %p", addr, (void*) (((uint64_t)addr)+size));
    int ret = memory_mark_pages_on((uint64_t)addr, size, write, execute, /*usermode=*/true);
    if (ret < 0)
        return ret;

#ifdef ASAN
    asan_unpoison_region((uintptr_t)addr, size);
#endif
    /* Keep the whole libgraphics reservation write-back cacheable. GEM mmap
     * requests change only the ranges they actually consume to write-combining
     * through memory_protect() below.
     *
     * The arenas deliberately skip the zero-fill: they are fresh memfd pages,
     * which are zero on first touch anyway, and writing the whole arena zero
     * would pin arena_size bytes of physical RAM (and cost a multi-GiB memset
     * at boot) for nothing. GEM buffer mappings replace arena slices outright. */
    bool is_egl_redirect_region = is_egl_redirect_data_region_base(addr, size);
    if (is_egl_redirect_region) {
        log_always("Reserved GEM arena %p (size %lu); fresh memfd pages, allocations marked WC on demand",
                   addr, (unsigned long)size);
    } else {
        memset(addr, 0, size);
    }
    return 0;
}

int memory_protect(void* addr, size_t size, bool read, bool write, bool execute) {
    if ((uintptr_t)addr < SHARED_MEM_ADDR + SHARED_MEM_SIZE &&
            SHARED_MEM_ADDR < (uintptr_t)addr + size) {
        /* [addr, addr+size) at least partially overlaps shared memory, should be impossible */
        return -PAL_ERROR_DENIED;
    }

    if (!read && !write && !execute) {
#ifdef ASAN
        asan_poison_region((uintptr_t)addr, size, ASAN_POISON_USER);
#endif
        memory_mark_pages_off((uint64_t)addr, size);
        return 0;
    }

#ifdef ASAN
    asan_unpoison_region((uintptr_t)addr, size);
#endif
    int ret = memory_mark_pages_on((uint64_t)addr, size, write, execute, /*usermode=*/true);
    if (ret < 0)
        return ret;

    if (is_egl_redirect_data_region_range(addr, size)) {
        ret = memory_mark_pages_write_combined((uint64_t)addr, size, /*mark=*/true);
        if (ret < 0) {
            log_error("Failed to mark GEM range at %p (size %lu) as write-combining: %d",
                      addr, (unsigned long)size, ret);
            BUG();
        }
    }
    return 0;
}

int memory_free(void* addr, size_t size) {
    if ((uintptr_t)addr < SHARED_MEM_ADDR + SHARED_MEM_SIZE &&
            SHARED_MEM_ADDR < (uintptr_t)addr + size) {
        /* [addr, addr+size) at least partially overlaps shared memory, should be impossible */
        return -PAL_ERROR_DENIED;
    }

#ifdef ASAN
    asan_poison_region((uintptr_t)addr, size, ASAN_POISON_USER);
#endif
    return memory_mark_pages_off((uint64_t)addr, size);
}

#ifdef __x86_64__
struct frame_alloc_record {
    uintptr_t next;
    uint64_t old_pte;
};

static bool is_frame_pool_frame(uintptr_t frame) {
    return g_frame_pool_bitmap && frame >= g_frame_pool_start &&
           frame < g_frame_pool_start + g_frame_pool_size &&
           IS_ALIGNED_POW2(frame, PAGE_SIZE);
}

static int address_space_restore_pte(struct memory_address_space* space, uintptr_t addr,
                                     uint64_t pte) {
    uint64_t* entry;
    int ret = address_space_get_leaf_entry(space, addr, /*make_private=*/true, &entry);
    if (ret < 0)
        return ret;
    *entry = pte;
    uint64_t current_root;
    __asm__ volatile("mov %%cr3, %0" : "=r"(current_root));
    if ((current_root & 0x00000ffffffff000UL) == space->root_phys)
        __asm__ volatile("invlpg (%0)" : : "r"(addr) : "memory");
    return 0;
}

__attribute_no_sanitize_address
int memory_address_space_alloc(struct memory_address_space* space, void* addr, size_t size,
                               bool read, bool write, bool execute) {
    if (!g_frame_pool_bitmap)
        return memory_alloc(addr, size, read, write, execute);
    if (is_egl_redirect_data_region_base(addr, size)) {
        int ret = memory_alloc(addr, size, read, write, execute);
        if (ret < 0)
            return ret;
        return memory_address_space_share_range(memory_address_space_boot(), space,
                                                (uintptr_t)addr, size);
    }
    if (!space || !IS_ALIGNED_POW2((uintptr_t)addr, PAGE_SIZE) || !size ||
            !IS_ALIGNED_POW2(size, PAGE_SIZE) || (uintptr_t)addr + size < (uintptr_t)addr)
        return -PAL_ERROR_INVAL;
    if ((uintptr_t)addr < SHARED_MEM_ADDR + SHARED_MEM_SIZE &&
            SHARED_MEM_ADDR < (uintptr_t)addr + size)
        return -PAL_ERROR_DENIED;

    if (!read && !write && !execute) {
        for (uintptr_t va = (uintptr_t)addr; va < (uintptr_t)addr + size; va += PAGE_SIZE) {
            uint64_t* entry;
            int ret = address_space_get_leaf_entry(space, va, /*make_private=*/false, &entry);
            if (ret < 0)
                return ret;
            uint64_t physical = *entry & 0x00000ffffffff000UL;
            ret = address_space_restore_pte(space, va, physical);
            if (ret < 0)
                return ret;
        }
        int ret = memory_address_space_flush_range((uintptr_t)addr, size);
        if (ret < 0)
            return ret;
#ifdef ASAN
        asan_poison_region((uintptr_t)addr, size, ASAN_POISON_USER);
#endif
        return 0;
    }

    /* Allocate all frames first and keep rollback data in each not-yet-mapped frame. */
    uintptr_t frame_list = 0;
    for (uintptr_t va = (uintptr_t)addr; va < (uintptr_t)addr + size; va += PAGE_SIZE) {
        uintptr_t frame;
        int ret = memory_frame_alloc(&frame);
        if (ret < 0) {
            while (frame_list) {
                struct frame_alloc_record* record = memory_frame_pointer(frame_list);
                uintptr_t next = record->next;
                memory_frame_free(frame_list);
                frame_list = next;
            }
            return ret;
        }

        uint64_t* old_pte;
        ret = address_space_get_leaf_entry(space, va, /*make_private=*/false, &old_pte);
        if (ret < 0) {
            memory_frame_free(frame);
            while (frame_list) {
                struct frame_alloc_record* record = memory_frame_pointer(frame_list);
                uintptr_t next = record->next;
                memory_frame_free(frame_list);
                frame_list = next;
            }
            return ret;
        }
        struct frame_alloc_record* record = memory_frame_pointer(frame);
        record->next = frame_list;
        record->old_pte = *old_pte;
        frame_list = frame;
    }

    uintptr_t frame = frame_list;
    size_t mapped_pages = 0;
    for (uintptr_t va = (uintptr_t)addr + size - PAGE_SIZE;; va -= PAGE_SIZE) {
        struct frame_alloc_record* record = memory_frame_pointer(frame);
        uintptr_t next = record->next;
        int ret = memory_address_space_map(space, va, frame, write, execute, /*usermode=*/true);
        if (ret < 0) {
            uintptr_t rollback_frame = frame_list;
            for (size_t i = 0; i < mapped_pages; i++) {
                struct frame_alloc_record* rollback_record = memory_frame_pointer(rollback_frame);
                uintptr_t rollback_next = rollback_record->next;
                uintptr_t rollback_va = (uintptr_t)addr + size - PAGE_SIZE - i * PAGE_SIZE;
                address_space_restore_pte(space, rollback_va, rollback_record->old_pte);
                rollback_frame = rollback_next;
            }
            if (mapped_pages && memory_address_space_flush_range((uintptr_t)addr, size) < 0)
                BUG();
            rollback_frame = frame_list;
            for (size_t i = 0; i < mapped_pages; i++) {
                struct frame_alloc_record* rollback_record = memory_frame_pointer(rollback_frame);
                uintptr_t rollback_next = rollback_record->next;
                memory_frame_free(rollback_frame);
                rollback_frame = rollback_next;
            }
            while (rollback_frame) {
                struct frame_alloc_record* rollback_record = memory_frame_pointer(rollback_frame);
                uintptr_t rollback_next = rollback_record->next;
                memory_frame_free(rollback_frame);
                rollback_frame = rollback_next;
            }
            return ret;
        }
        mapped_pages++;
        frame = next;
        if (va == (uintptr_t)addr)
            break;
    }

    int ret = memory_address_space_flush_range((uintptr_t)addr, size);
    if (ret < 0)
        return ret;

#ifdef ASAN
    asan_unpoison_region((uintptr_t)addr, size);
#endif
    memset(addr, 0, size);
    return 0;
}

__attribute_no_sanitize_address
int memory_address_space_free(struct memory_address_space* space, void* addr, size_t size) {
    if (!g_frame_pool_bitmap)
        return memory_free(addr, size);
    if (is_egl_redirect_data_region_range(addr, size)) {
        int ret = memory_free(addr, size);
        if (ret < 0)
            return ret;
        return memory_address_space_share_range(memory_address_space_boot(), space,
                                                (uintptr_t)addr, size);
    }
    if (!space || !IS_ALIGNED_POW2((uintptr_t)addr, PAGE_SIZE) || !size ||
            !IS_ALIGNED_POW2(size, PAGE_SIZE) || (uintptr_t)addr + size < (uintptr_t)addr)
        return -PAL_ERROR_INVAL;
    if ((uintptr_t)addr < SHARED_MEM_ADDR + SHARED_MEM_SIZE &&
            SHARED_MEM_ADDR < (uintptr_t)addr + size)
        return -PAL_ERROR_DENIED;

    const uint64_t addr_mask = 0x00000ffffffff000UL;
    for (uintptr_t va = (uintptr_t)addr; va < (uintptr_t)addr + size; va += PAGE_SIZE) {
        uint64_t* entry;
        int ret = address_space_get_leaf_entry(space, va, /*make_private=*/false, &entry);
        if (ret < 0)
            return ret;
        uintptr_t frame = *entry & addr_mask;
        ret = address_space_get_leaf_entry(space, va, /*make_private=*/true, &entry);
        if (ret < 0)
            return ret;
        *entry = frame;

        uint64_t current_root;
        __asm__ volatile("mov %%cr3, %0" : "=r"(current_root));
    if ((current_root & addr_mask) == space->root_phys)
            __asm__ volatile("invlpg (%0)" : : "r"(va) : "memory");
    }
    int ret = memory_address_space_flush_range((uintptr_t)addr, size);
    if (ret < 0)
        return ret;

    /* Do not return a physical frame to the pool until remote vCPUs have dropped cached mappings
     * to it. The not-present PTEs keep the frame addresses available for this second pass. */
    for (uintptr_t va = (uintptr_t)addr; va < (uintptr_t)addr + size; va += PAGE_SIZE) {
        uint64_t* entry;
        ret = address_space_get_leaf_entry(space, va, /*make_private=*/false, &entry);
        if (ret < 0)
            return ret;
        uintptr_t frame = *entry & addr_mask;
        if (is_frame_pool_frame(frame)) {
            ret = memory_frame_free(frame);
            if (ret < 0)
                return ret;
        }
    }
#ifdef ASAN
    asan_poison_region((uintptr_t)addr, size, ASAN_POISON_USER);
#endif
    return 0;
}

__attribute_no_sanitize_address
int memory_address_space_protect(struct memory_address_space* space, void* addr, size_t size,
                                 bool read, bool write, bool execute) {
    if (!g_frame_pool_bitmap)
        return memory_protect(addr, size, read, write, execute);
    if (is_egl_redirect_data_region_range(addr, size)) {
        int ret = memory_protect(addr, size, read, write, execute);
        if (ret < 0)
            return ret;
        return memory_address_space_share_range(memory_address_space_boot(), space,
                                                (uintptr_t)addr, size);
    }
    if (!space || !IS_ALIGNED_POW2((uintptr_t)addr, PAGE_SIZE) || !size ||
            !IS_ALIGNED_POW2(size, PAGE_SIZE) || (uintptr_t)addr + size < (uintptr_t)addr)
        return -PAL_ERROR_INVAL;
    if ((uintptr_t)addr < SHARED_MEM_ADDR + SHARED_MEM_SIZE &&
            SHARED_MEM_ADDR < (uintptr_t)addr + size)
        return -PAL_ERROR_DENIED;

    const uint64_t addr_mask = 0x00000ffffffff000UL;
    for (uintptr_t va = (uintptr_t)addr; va < (uintptr_t)addr + size; va += PAGE_SIZE) {
        uint64_t* entry;
        int ret = address_space_get_leaf_entry(space, va, /*make_private=*/false, &entry);
        if (ret < 0)
            return ret;
        uintptr_t frame = *entry & addr_mask;
        if (!read && !write && !execute) {
            ret = address_space_restore_pte(space, va, frame);
        } else {
            ret = memory_address_space_map(space, va, frame, write, execute, /*usermode=*/true);
        }
        if (ret < 0)
            return ret;
    }
    int ret = memory_address_space_flush_range((uintptr_t)addr, size);
    if (ret < 0)
        return ret;
#ifdef ASAN
    if (read || write || execute)
        asan_unpoison_region((uintptr_t)addr, size);
    else
        asan_poison_region((uintptr_t)addr, size, ASAN_POISON_USER);
#endif
    return 0;
}
#endif
