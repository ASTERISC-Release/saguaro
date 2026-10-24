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
/* References held on a frame by page-table leaves of process roots (a forked child shares its
 * parent's frames until it replaces them; MAP_SHARED memory stays shared). Other frame users
 * (page tables, alias-only objects) hold the single reference set by memory_frame_alloc(). */
static uint16_t* g_frame_refcount;
static spinlock_t g_frame_pool_lock = INIT_SPINLOCK_UNLOCKED;

/* The pool is also used by the page-fault handler (demand-zero pages), which runs with interrupts
 * disabled: restore the caller's interrupt state instead of enabling interrupts on unlock (an
 * interrupt nested on the shared IST stack would overwrite the handler's frame). */
static inline uint64_t frame_pool_lock(void) {
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(flags) : : "memory");
    spinlock_lock(&g_frame_pool_lock);
    return flags;
}

static inline void frame_pool_unlock(uint64_t flags) {
    spinlock_unlock(&g_frame_pool_lock);
    if (flags & 0x200)
        __asm__ volatile("sti" : : : "memory");
}

/* A supervisor-only direct-map window gives every root stable access to physical frames and
 * page tables, even when a process remaps their identity addresses. */
#define FRAME_POOL_ALIAS_BASE 0xffff800000000000UL
#define FRAME_POOL_ALIAS_SIZE (512UL * 1024 * 1024 * 1024)
#define PAGE_2M_SIZE (2UL * 1024 * 1024)
#define PTE_PRESENT 0x001UL
#define PTE_WRITE   0x002UL
#define PTE_HUGE    0x080UL
#define PTE_USER    0x004UL
#define PTE_NX      (1UL << 63)
/* Software bit of a non-present leaf (ignored by the hardware): a demand-zero page whose access
 * rights are the entry's W and NX bits; see memory_address_space_handle_fault(). */
#define PTE_LAZY    (1UL << 9)
/* Software bits of a leaf that maps a frame shared with a file's cache (a MAP_PRIVATE file
 * mapping, Linux's page-cache semantics): the page is read-only in the hardware bits and a write
 * must first copy the frame (memory_address_space_map_cow_frame()). PTE_COW_WRITABLE says the
 * mapping allows writes, so the fault handler copies; without it a write is a genuine fault. */
#define PTE_COW          (1UL << 10)
#define PTE_COW_WRITABLE (1UL << 11)

static struct memory_address_space g_boot_address_space;
#endif

#ifdef PAL_HOST_SEV
/* AMD SEV (pal/src/host/vm/sev/): the C-bit as a page-table-entry mask, set by vm_bootloader.S.
 * Private (encrypted) memory has it set; the shared heap and the MMIO hole, which the host must
 * read and write, have it clear. Guest page tables must be private: hardware walks them with the
 * guest key whatever the C-bit says. */
#include "sev.h"
static uint64_t sev_c_bit_for_phys(uint64_t phys) {
    if (SHARED_MEM_ADDR <= phys && phys < SHARED_MEM_ADDR + SHARED_MEM_SIZE)
        return 0;
    if (PCI_HOLE_ADDR <= phys && phys < PCI_HOLE_ADDR + PCI_HOLE_SIZE)
        return 0;
    /* g_sev_extra_shared_page is 0 unless the SEV-ES/SNP GHCB page was set up: without this
     * check, physical 0 (the first 2 MB, where the PAL image and its .process_data live) would
     * match it and be mapped unencrypted under plain SEV */
    if (g_sev_extra_shared_page && phys == g_sev_extra_shared_page)
        return 0;
    return g_sev_c_bit_mask;
}
/* the multiprocess support's tables and frame metadata must be private under SEV */
#define PTE_ENC g_sev_c_bit_mask
#define KERNEL_META_ALLOC(size) sev_alloc_private(size)
#define SEV_VALIDATE_FRAME(frame) sev_validate_frame(frame)
#else
#define PTE_ENC 0UL
#define KERNEL_META_ALLOC(size) memory_get_shared_region(size)
#define SEV_VALIDATE_FRAME(frame) 0
#endif

#ifdef __x86_64__

void* memory_frame_pointer(uintptr_t frame) {
    if (g_frame_alias_mapped_size && frame < g_frame_alias_mapped_size)
        return (void*)(FRAME_POOL_ALIAS_BASE + frame);
    return (void*)frame;
}

bool memory_frame_is_alias_pointer(const void* pointer) {
    uintptr_t addr = (uintptr_t)pointer;
    return g_frame_alias_mapped_size && addr >= FRAME_POOL_ALIAS_BASE &&
           addr < FRAME_POOL_ALIAS_BASE + g_frame_alias_mapped_size;
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

    uint64_t* pdpt = KERNEL_META_ALLOC(PAGE_SIZE);
    size_t pd_count = UDIV_ROUND_UP(mapped_size, 1024UL * 1024 * 1024);
    uint64_t* pd_tables = KERNEL_META_ALLOC(pd_count * PAGE_SIZE);
    if (!pdpt || !pd_tables)
        return -PAL_ERROR_NOMEM;

    for (size_t pdpt_idx = 0; pdpt_idx < pd_count; pdpt_idx++) {
        uint64_t* pd = (uint64_t*)((uintptr_t)pd_tables + pdpt_idx * PAGE_SIZE);
        pdpt[pdpt_idx] = (uintptr_t)pd | PTE_PRESENT | PTE_WRITE | PTE_ENC;
        size_t remaining = (mapped_size - pdpt_idx * 1024UL * 1024 * 1024) / PAGE_2M_SIZE;
        size_t pages_in_pd = MIN(512UL, remaining);
        for (size_t i = 0; i < pages_in_pd; i++) {
            uintptr_t physical = pdpt_idx * 1024UL * 1024 * 1024 + i * PAGE_2M_SIZE;
            pd[i] = physical | PTE_PRESENT | PTE_WRITE | PTE_HUGE | PTE_NX;
#ifdef PAL_HOST_SEV
            pd[i] |= sev_c_bit_for_phys(physical); /* the shared heap and MMIO stay shared */
#endif
        }
    }

    size_t pml4_idx = (FRAME_POOL_ALIAS_BASE >> 39) & 0x1ff;
    uint64_t* pml4 = (uint64_t*)g_pml4_table_base;
    if (pml4[pml4_idx] & PTE_PRESENT)
        return -PAL_ERROR_DENIED;
    pml4[pml4_idx] = (uintptr_t)pdpt | PTE_PRESENT | PTE_WRITE | PTE_ENC;
    g_frame_alias_mapped_size = mapped_size;
    g_frame_pool_bitmap = memory_frame_pointer((uintptr_t)g_frame_pool_bitmap);
    g_frame_owner_next = memory_frame_pointer((uintptr_t)g_frame_owner_next);
    g_frame_owner_kind = memory_frame_pointer((uintptr_t)g_frame_owner_kind);
    g_frame_refcount = memory_frame_pointer((uintptr_t)g_frame_refcount);
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
    uint8_t* bitmap = KERNEL_META_ALLOC(bitmap_size);
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
    g_frame_owner_next = KERNEL_META_ALLOC(owner_next_size);
    g_frame_owner_kind = KERNEL_META_ALLOC(owner_kind_size);
    size_t refcount_size = frame_count * sizeof(*g_frame_refcount);
    g_frame_refcount = KERNEL_META_ALLOC(refcount_size);
    if (!g_frame_owner_next || !g_frame_owner_kind || !g_frame_refcount)
        return -PAL_ERROR_NOMEM;
    memset(g_frame_owner_next, 0, owner_next_size);
    memset(g_frame_owner_kind, 0, owner_kind_size);
    memset(g_frame_refcount, 0, refcount_size);
    ret = memory_frame_pool_map_alias((uintptr_t)g_pal_public_state.memory_address_end);
    if (ret < 0)
        return ret;
    // log_always("Private frame pool: [%p, %p), %zu frames", (void*)addr, (void*)(addr + size),
    //            frame_count);
    return 0;
}

int memory_frame_alloc(uintptr_t* out_frame) {
    if (!out_frame || !g_frame_pool_bitmap)
        return -PAL_ERROR_INVAL;

    uint64_t pool_irq_flags = frame_pool_lock();
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
#ifdef ASAN
        asan_unpoison_region(frame, PAGE_SIZE);
#endif
        if (SEV_VALIDATE_FRAME(frame) < 0) {
            g_frame_pool_bitmap[i / 8] &= ~mask;
            frame_pool_unlock(pool_irq_flags);
            return -PAL_ERROR_DENIED;
        }
        _real_memset(memory_frame_pointer(frame), 0, PAGE_SIZE);
        *out_frame = frame;
        frame_pool_unlock(pool_irq_flags);
        return 0;
    }
    frame_pool_unlock(pool_irq_flags);
    return -PAL_ERROR_NOMEM;
}

int memory_frame_free(uintptr_t frame) {
    if (!g_frame_pool_bitmap || frame < g_frame_pool_start ||
            frame >= g_frame_pool_start + g_frame_pool_size ||
            !IS_ALIGNED_POW2(frame, PAGE_SIZE))
        return -PAL_ERROR_INVAL;

    size_t i = (frame - g_frame_pool_start) / PAGE_SIZE;
    uint8_t mask = 1U << (i % 8);
    uint64_t pool_irq_flags = frame_pool_lock();
    if (!(g_frame_pool_bitmap[i / 8] & mask)) {
        frame_pool_unlock(pool_irq_flags);
        return -PAL_ERROR_INVAL;
    }
    g_frame_pool_bitmap[i / 8] &= ~mask;
    g_frame_owner_next[i] = 0;
    g_frame_owner_kind[i] = 0;
    g_frame_refcount[i] = 0;
    if (i < g_frame_pool_next)
        g_frame_pool_next = i;
#ifdef ASAN
    asan_poison_region(frame, PAGE_SIZE, ASAN_POISON_USER);
#endif
    frame_pool_unlock(pool_irq_flags);
    return 0;
}

/* Allocates `size` bytes of physically contiguous frames and maps them into `space` at `addr`
 * (read-write, user mode); `*out_phys` is the first frame's guest-physical address. For the
 * LibOS's graphics bounce buffers: the host listener reads guest RAM by physical address, so what
 * the LibOS hands it from a demand-paged process must sit in frames it knows the address of
 * (dev docs: proton-vm/design-gpu.md). Without a frame pool or a process address space
 * (the single-process VM) memory is identity-mapped and a plain allocation serves. */
int memory_address_space_alloc_contiguous(struct memory_address_space* space, void* addr,
                                          size_t size, bool execute, uintptr_t* out_phys) {
    if (!addr || !size || !out_phys || !IS_ALIGNED_POW2((uintptr_t)addr, PAGE_SIZE) ||
            !IS_ALIGNED_POW2(size, PAGE_SIZE))
        return -PAL_ERROR_INVAL;
    if (!g_frame_pool_bitmap || !space) {
        int ret = memory_alloc(addr, size, /*read=*/true, /*write=*/true, execute);
        if (ret < 0)
            return ret;
        *out_phys = (uintptr_t)addr;
        return 0;
    }
    void* alias;
    int ret = memory_frame_alloc_contiguous(size, &alias);
    if (ret < 0)
        return ret;
    uintptr_t frame = g_frame_alias_mapped_size && (uintptr_t)alias >= FRAME_POOL_ALIAS_BASE
                      ? (uintptr_t)alias - FRAME_POOL_ALIAS_BASE : (uintptr_t)alias;
    for (size_t done = 0; done < size; done += PAGE_SIZE) {
        ret = memory_address_space_map_owned(space, (uintptr_t)addr + done, frame + done,
                                             /*write=*/true, execute, /*usermode=*/true);
        if (ret < 0) {
            if (done)
                (void)memory_address_space_free(space, addr, done);
            for (size_t rest = done; rest < size; rest += PAGE_SIZE)
                memory_frame_free(frame + rest);
            return ret;
        }
    }
    ret = memory_address_space_flush_range((uintptr_t)addr, size);
    if (ret < 0)
        return ret;
    *out_phys = frame;
    return 0;
}

int memory_frame_alloc_contiguous(size_t size, void** out_alias) {
    if (!g_frame_pool_bitmap || !size || !out_alias || size > SIZE_MAX - PAGE_SIZE + 1)
        return -PAL_ERROR_INVAL;

    size_t pages = UDIV_ROUND_UP(size, PAGE_SIZE);
    size_t frame_count = g_frame_pool_size / PAGE_SIZE;
    if (!pages || pages > frame_count)
        return -PAL_ERROR_NOMEM;

    uint64_t pool_irq_flags = frame_pool_lock();
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
            if (SEV_VALIDATE_FRAME(frame + j * PAGE_SIZE) < 0) {
                frame_pool_unlock(pool_irq_flags);
                return -PAL_ERROR_DENIED;
            }
            _real_memset(memory_frame_pointer(frame + j * PAGE_SIZE), 0, PAGE_SIZE);
        }
        g_frame_pool_next = (start + pages) % frame_count;
        *out_alias = memory_frame_pointer(frame);
        frame_pool_unlock(pool_irq_flags);
        return 0;
    }
    frame_pool_unlock(pool_irq_flags);
    return -PAL_ERROR_NOMEM;
}

int memory_frame_free_contiguous(void* alias, size_t size) {
    if (!g_frame_pool_bitmap || !memory_frame_is_alias_pointer(alias) || !size ||
            size > SIZE_MAX - PAGE_SIZE + 1 ||
            !IS_ALIGNED_POW2((uintptr_t)alias, PAGE_SIZE))
        return -PAL_ERROR_INVAL;

    uintptr_t frame = (uintptr_t)alias - FRAME_POOL_ALIAS_BASE;
    size_t pages = UDIV_ROUND_UP(size, PAGE_SIZE);
    if (frame < g_frame_pool_start || pages > g_frame_pool_size / PAGE_SIZE ||
            frame - g_frame_pool_start > g_frame_pool_size - pages * PAGE_SIZE)
        return -PAL_ERROR_INVAL;

    size_t start = (frame - g_frame_pool_start) / PAGE_SIZE;
    uint64_t pool_irq_flags = frame_pool_lock();
    for (size_t j = 0; j < pages; j++) {
        size_t i = start + j;
        if (!(g_frame_pool_bitmap[i / 8] & (1U << (i % 8)))) {
            frame_pool_unlock(pool_irq_flags);
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
    frame_pool_unlock(pool_irq_flags);
    return 0;
}

static bool is_frame_pool_frame(uintptr_t frame) {
    return g_frame_pool_bitmap && frame >= g_frame_pool_start &&
           frame < g_frame_pool_start + g_frame_pool_size &&
           IS_ALIGNED_POW2(frame, PAGE_SIZE);
}

/* Takes another leaf reference on an allocated pool frame; other frames are ignored. */
static void memory_frame_get(uintptr_t frame) {
    if (!is_frame_pool_frame(frame))
        return;
    size_t i = (frame - g_frame_pool_start) / PAGE_SIZE;
    uint64_t pool_irq_flags = frame_pool_lock();
    if (!(g_frame_pool_bitmap[i / 8] & (1U << (i % 8))) || g_frame_refcount[i] == UINT16_MAX) {
        frame_pool_unlock(pool_irq_flags);
        log_error("Invalid reference to frame %p", (void*)frame);
        BUG();
    }
    g_frame_refcount[i]++;
    frame_pool_unlock(pool_irq_flags);
}

/* Drops a leaf reference; the last one returns the frame to the pool. The caller must have
 * flushed stale translations of the leaf it removed. */
static void memory_frame_put(uintptr_t frame) {
    if (!is_frame_pool_frame(frame))
        return;
    size_t i = (frame - g_frame_pool_start) / PAGE_SIZE;
    uint64_t pool_irq_flags = frame_pool_lock();
    if (!(g_frame_pool_bitmap[i / 8] & (1U << (i % 8))) || !g_frame_refcount[i]) {
        frame_pool_unlock(pool_irq_flags);
        log_error("Unbalanced release of frame %p", (void*)frame);
        BUG();
    }
    bool last = --g_frame_refcount[i] == 0;
    if (last)
        g_frame_refcount[i] = 1; /* memory_frame_free() below releases it */
    frame_pool_unlock(pool_irq_flags);
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
    uint64_t pool_irq_flags = frame_pool_lock();
    for (size_t i = 0; i < frame_count; i++)
        count += !!(g_frame_pool_bitmap[i / 8] & (1U << (i % 8)));
    frame_pool_unlock(pool_irq_flags);
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
        *out_page = KERNEL_META_ALLOC(PAGE_SIZE);
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
        __atomic_store_n(entry, (*entry & ~addr_mask) | (memory_frame_from_pointer(copy) & addr_mask),
                         __ATOMIC_RELEASE);
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

/* Whether a leaf holds a reference on its frame: every pool frame mapped at a user address does;
 * the supervisor identity mapping of the pool itself (inherited from the boot tables) does not. */
static bool leaf_holds_reference(uintptr_t va, uint64_t entry) {
    uintptr_t frame = entry & 0x00000ffffffff000UL;
    return is_frame_pool_frame(frame) && frame != va;
}

/* Descendants get private copies of every page-table page. Their leaves keep the same physical
 * mappings; process-private memory is copied separately by the caller. */
static int address_space_copy_table_tree(struct memory_address_space* target, uint64_t* table,
                                         unsigned int level, uintptr_t va_base) {
    const uint64_t addr_mask = 0x00000ffffffff000UL;
    size_t alias_pml4_idx = (FRAME_POOL_ALIAS_BASE >> 39) & 0x1ff;
    for (size_t i = 0; i < 512; i++) {
        uint64_t entry = table[i];
        if (level == 1) {
            /* the child's leaf shares the frame (present or kept in a PROT_NONE entry) */
            if (leaf_holds_reference(va_base + i * PAGE_SIZE, entry))
                memory_frame_get(entry & addr_mask);
            continue;
        }
        if (!(entry & PTE_PRESENT) ||
                ((level == 2 || level == 3) && (entry & PTE_HUGE)) ||
                (level == 4 && i == alias_pml4_idx))
            continue;

        uint64_t* child;
        int ret = address_space_alloc_table_page(target, &child);
        if (ret < 0) {
            /* entries from `i` on still point to the source's tables: drop them, so that the
             * caller's destroy releases only what this copy referenced */
            memset(&table[i], 0, (512 - i) * sizeof(*table));
            return ret;
        }
        memcpy(child, memory_frame_pointer(entry & addr_mask), PAGE_SIZE);
        table[i] = (entry & ~addr_mask) | (memory_frame_from_pointer(child) & addr_mask);
        uintptr_t shift = level == 4 ? 39 : level == 3 ? 30 : 21;
        ret = address_space_copy_table_tree(target, child, level - 1, va_base + (i << shift));
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
    ret = address_space_copy_table_tree(out_space, root, 4, 0);
    if (ret < 0)
        memory_address_space_destroy(out_space);
    return ret;
}

/* Drops the leaf references of the tables this space owns. Tables still shared with the boot root
 * (not pool frames) are not descended into: they hold no references. */
static void address_space_put_leaves(uint64_t* table, unsigned int level, uintptr_t va_base) {
    const uint64_t addr_mask = 0x00000ffffffff000UL;
    size_t alias_pml4_idx = (FRAME_POOL_ALIAS_BASE >> 39) & 0x1ff;
    for (size_t i = 0; i < 512; i++) {
        uint64_t entry = table[i];
        uintptr_t frame = entry & addr_mask;
        if (level == 1) {
            if (leaf_holds_reference(va_base + i * PAGE_SIZE, entry))
                memory_frame_put(frame);
            continue;
        }
        if (!(entry & PTE_PRESENT) || ((level == 2 || level == 3) && (entry & PTE_HUGE)) ||
                (level == 4 && i == alias_pml4_idx) || !is_frame_pool_frame(frame))
            continue;
        uintptr_t shift = level == 4 ? 39 : level == 3 ? 30 : 21;
        address_space_put_leaves(memory_frame_pointer(frame), level - 1, va_base + (i << shift));
    }
}

size_t memory_address_space_destroy(struct memory_address_space* space) {
    if (!space)
        return 0;
    if (space->root && space->root_phys && g_frame_pool_bitmap)
        address_space_put_leaves(space->root, 4, 0);
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
        uint64_t* target_entry;
        ret = address_space_get_leaf_entry(target, va, /*make_private=*/true, &target_entry);
        if (ret < 0) {
            memory_frame_free(frame);
            return ret;
        }
        uintptr_t old_frame = *target_entry & 0x00000ffffffff000UL;
        ret = memory_address_space_map(target, va, frame, /*write=*/true, /*execute=*/false,
                                       /*usermode=*/false);
        if (ret < 0) {
            memory_frame_free(frame);
            return ret;
        }
        /* the new leaf owns `frame`; the reference of the replaced leaf goes after the flush */
        if (is_frame_pool_frame(old_frame)) {
            ret = memory_address_space_flush_range(va, PAGE_SIZE);
            if (ret < 0)
                return ret;
            memory_frame_put(old_frame);
        }
    }
    return memory_address_space_flush_range(addr, size);
}

/* Serializes the creation and the private copy of page tables (of every address space): two vCPUs
 * that map into the same missing or still shared table must not both install a table of their own,
 * the entries written into the one that lost would be lost (the page then faults with no entry).
 * Never taken in interrupt context; the holder only allocates and fills the table. */
static spinlock_t g_address_space_tables_lock = INIT_SPINLOCK_UNLOCKED;

/* The table `entry` points to, on a walk of `space`. With `make_private`, a table the base (the
 * boot tables) still shares is copied first, and a missing one (an address beyond what the boot
 * tables cover, up to MEMORY_ADDRESS_SPACE_LIMIT: the application's range of pal_main.c) is
 * created, zeroed; without it, a missing table is -PAL_ERROR_INVAL. `base_entry` is the base's
 * entry at the same place, 0 when the base has none. */
static int address_space_get_child_locked(uint64_t* entry, uint64_t base_entry,
                                          struct memory_address_space* space, bool make_private,
                                          uint64_t** out_child) {
    const uint64_t addr_mask = 0x00000ffffffff000UL;
    if (!(*entry & PTE_PRESENT)) {
        if (!make_private)
            return -PAL_ERROR_INVAL;
        uint64_t* page;
        int ret = address_space_alloc_table_page(space, &page);
        if (ret < 0)
            return ret;
        memset(page, 0, PAGE_SIZE);
        __atomic_store_n(entry, (memory_frame_from_pointer(page) & addr_mask) | PTE_PRESENT |
                                PTE_WRITE | PTE_USER | PTE_ENC, __ATOMIC_RELEASE);
        *out_child = page;
        return 0;
    }
    if (make_private && (base_entry & PTE_PRESENT) &&
            (*entry & addr_mask) == (base_entry & addr_mask))
        return address_space_get_private_child(entry, base_entry, space, out_child);
    *out_child = memory_frame_pointer(*entry & addr_mask);
    return 0;
}

static int address_space_get_child(uint64_t* entry, uint64_t base_entry,
                                   struct memory_address_space* space, bool make_private,
                                   uint64_t** out_child) {
    const uint64_t addr_mask = 0x00000ffffffff000UL;
    uint64_t cur = __atomic_load_n(entry, __ATOMIC_ACQUIRE);
    bool needs_table = !(cur & PTE_PRESENT) ||
                       ((base_entry & PTE_PRESENT) && (cur & addr_mask) == (base_entry & addr_mask));
    if (!make_private || !needs_table)
        return address_space_get_child_locked(entry, base_entry, space, make_private, out_child);

    /* a table to create or to copy: re-checked under the lock, another vCPU may have done it */
    spinlock_lock(&g_address_space_tables_lock);
    int ret = address_space_get_child_locked(entry, base_entry, space, make_private, out_child);
    spinlock_unlock(&g_address_space_tables_lock);
    return ret;
}

static int address_space_get_leaf_entry(struct memory_address_space* space, uintptr_t addr,
                                        bool make_private, uint64_t** out_entry) {
    const uint64_t addr_mask = 0x00000ffffffff000UL;
    if (!space || !space->root || !space->root_phys ||
            addr >= MEMORY_ADDRESS_SPACE_LIMIT || !IS_ALIGNED_POW2(addr, PAGE_SIZE))
        return -PAL_ERROR_INVAL;

    size_t pml4_idx = (addr >> 39) & 0x1ff;
    size_t pdpt_idx = (addr >> 30) & 0x1ff;
    size_t pd_idx = (addr >> 21) & 0x1ff;
    size_t pt_idx = (addr >> 12) & 0x1ff;

    uint64_t base_entry = space->base_root ? space->base_root[pml4_idx] : 0;
    uint64_t* pdpt;
    int ret = address_space_get_child(&space->root[pml4_idx], base_entry, space, make_private,
                                      &pdpt);
    if (ret < 0)
        return ret;
    uint64_t* base_pdpt = (base_entry & PTE_PRESENT) ? memory_frame_pointer(base_entry & addr_mask)
                                                     : NULL;

    base_entry = base_pdpt ? base_pdpt[pdpt_idx] : 0;
    if ((pdpt[pdpt_idx] | base_entry) & PTE_HUGE)
        return -PAL_ERROR_INVAL; /* splitting a 1-GiB page is not implemented */
    uint64_t* pd;
    ret = address_space_get_child(&pdpt[pdpt_idx], base_entry, space, make_private, &pd);
    if (ret < 0)
        return ret;
    uint64_t* base_pd = (base_entry & PTE_PRESENT) ? memory_frame_pointer(base_entry & addr_mask)
                                                   : NULL;

    base_entry = base_pd ? base_pd[pd_idx] : 0;
    if ((pd[pd_idx] | base_entry) & PTE_HUGE)
        return -PAL_ERROR_INVAL; /* splitting a 2-MiB page is not implemented */
    uint64_t* pt;
    ret = address_space_get_child(&pd[pd_idx], base_entry, space, make_private, &pt);
    if (ret < 0)
        return ret;

    *out_entry = &pt[pt_idx];
    return 0;
}

/* For walks over a range that create nothing: the leaf entry of `va` (returns 0), or 1 when no
 * table covers it, with `*out_next` the first address past the missing table's reach (a
 * reservation of many GiB beyond the boot tables is walked in 1-GiB steps). */
static int address_space_find_leaf_entry(struct memory_address_space* space, uintptr_t va,
                                         uint64_t** out_entry, uintptr_t* out_next) {
    const uint64_t addr_mask = 0x00000ffffffff000UL;
    uint64_t* table = space->root;
    for (unsigned int shift = 39; shift > 12; shift -= 9) {
        uint64_t entry = table[(va >> shift) & 0x1ff];
        if (!(entry & PTE_PRESENT)) {
            *out_next = ALIGN_DOWN(va, 1UL << shift) + (1UL << shift);
            return 1;
        }
        if (entry & PTE_HUGE)
            return -PAL_ERROR_INVAL;
        table = memory_frame_pointer(entry & addr_mask);
    }
    *out_entry = &table[(va >> 12) & 0x1ff];
    return 0;
}

static int address_space_present_leaf(struct memory_address_space* space, uintptr_t va,
                                      uint64_t* out_entry) {
    uint64_t* entry;
    uintptr_t next;
    int ret = address_space_find_leaf_entry(space, va, &entry, &next);
    if (ret != 0 || !(*entry & PTE_PRESENT))
        return -PAL_ERROR_INVAL;
    *out_entry = *entry;
    return 0;
}

__attribute_no_sanitize_address
int memory_address_space_map(struct memory_address_space* space, uintptr_t addr,
                             uintptr_t frame, bool write, bool execute, bool usermode) {
    const uint64_t addr_mask = 0x00000ffffffff000UL;
    if (!space || !space->root || !space->root_phys ||
            addr >= MEMORY_ADDRESS_SPACE_LIMIT || !IS_ALIGNED_POW2(addr, PAGE_SIZE) ||
            !IS_ALIGNED_POW2(frame, PAGE_SIZE) || (frame & ~addr_mask))
        return -PAL_ERROR_INVAL;

    uint64_t* pte;
    int ret = address_space_get_leaf_entry(space, addr, /*make_private=*/true, &pte);
    if (ret < 0)
        return ret;

    uint64_t flags = 1UL | (write ? 1UL << 1 : 0) | (usermode ? 1UL << 2 : 0) |
                     (execute ? 0 : 1UL << 63);
    *pte = frame | flags | PTE_ENC;

    uint64_t current_root;
    __asm__ volatile("mov %%cr3, %0" : "=r"(current_root));
    if ((current_root & addr_mask) == space->root_phys)
        __asm__ volatile("invlpg (%0)" : : "r"(addr) : "memory");
    return 0;
}

int memory_address_space_map_owned(struct memory_address_space* space, uintptr_t addr,
                                   uintptr_t frame, bool write, bool execute, bool usermode) {
    uint64_t* pte;
    int ret = address_space_get_leaf_entry(space, addr, /*make_private=*/true, &pte);
    if (ret < 0)
        return ret;
    uintptr_t old_frame = *pte & 0x00000ffffffff000UL;
    ret = memory_address_space_map(space, addr, frame, write, execute, usermode);
    if (ret < 0)
        return ret;
    if (is_frame_pool_frame(old_frame) && old_frame != frame) {
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

int memory_address_space_alias_device(struct memory_address_space* space, uintptr_t dst,
                                      uintptr_t src, size_t size, bool write, bool execute) {
    const uint64_t addr_mask = 0x00000ffffffff000UL;
    const uint64_t cache_bits = (1UL << 3) | (1UL << 4) | (1UL << 7); /* PWT, PCD, PAT */
    if (!g_frame_pool_bitmap || !space || !size || !IS_ALIGNED_POW2(dst, PAGE_SIZE) ||
            !IS_ALIGNED_POW2(src, PAGE_SIZE) || !IS_ALIGNED_POW2(size, PAGE_SIZE) ||
            dst + size < dst || src + size < src || (dst < src + size && src < dst + size))
        return -PAL_ERROR_INVAL;
    if (dst < SHARED_MEM_ADDR + SHARED_MEM_SIZE && SHARED_MEM_ADDR < dst + size)
        return -PAL_ERROR_DENIED;
    /* the source must be present device memory throughout: pool frames carry reference counts
     * that this mapping does not take, so they are refused */
    for (size_t off = 0; off < size; off += PAGE_SIZE) {
        uint64_t entry;
        int ret = address_space_present_leaf(space, src + off, &entry);
        if (ret < 0)
            return ret;
        if (is_frame_pool_frame(entry & addr_mask))
            return -PAL_ERROR_INVAL;
    }
    for (size_t off = 0; off < size; off += PAGE_SIZE) {
        uint64_t src_entry;
        int ret = address_space_present_leaf(space, src + off, &src_entry);
        if (ret < 0)
            return ret;
        ret = memory_address_space_map_owned(space, dst + off, src_entry & addr_mask, write,
                                             execute, /*usermode=*/true);
        if (ret < 0)
            return ret;
        uint64_t* dst_entry;
        ret = address_space_get_leaf_entry(space, dst + off, /*make_private=*/false, &dst_entry);
        if (ret < 0)
            return ret;
        *dst_entry = (*dst_entry & ~cache_bits) | (src_entry & cache_bits);
    }
    return memory_address_space_flush_range(dst, size);
}

/* Copies a frame-pool frame into a fresh one (for copy-on-write). */
static int cow_copy_frame(uintptr_t frame, uintptr_t* out_copy) {
    uintptr_t copy;
    int ret = memory_frame_alloc(&copy);
    if (ret < 0)
        return ret;
    memcpy(memory_frame_pointer(copy), memory_frame_pointer(frame), PAGE_SIZE);
    *out_copy = copy;
    return 0;
}

int memory_address_space_map_cow_frame(struct memory_address_space* space, uintptr_t addr,
                                       uintptr_t frame, bool writable, bool execute) {
    int ret = memory_address_space_map_shared_frame(space, addr, frame, /*write=*/false, execute);
    if (ret < 0)
        return ret;
    uint64_t* pte;
    ret = address_space_get_leaf_entry(space, addr, /*make_private=*/true, &pte);
    if (ret < 0)
        return ret;
    *pte |= PTE_COW | (writable ? PTE_COW_WRITABLE : 0);
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
        uintptr_t old_frame = *target_pte & addr_mask;
        if (old_frame != (entry & addr_mask)) {
            if (leaf_holds_reference(va, *target_pte)) {
                /* releasing it would need a flush first; no caller replaces owned frames here */
                return -PAL_ERROR_DENIED;
            }
            if (leaf_holds_reference(va, entry))
                memory_frame_get(entry & addr_mask);
        }
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

/* The comm page of the graphics redirection (libos/src/graphics, SYS_COMMS_ADDR): libgraphics maps it
 * at this guest-physical address and the host listener reads it there, so a process address space
 * maps it identically, like the data arenas. */
#define GRAPHICS_COMM_ADDR 0xf00000UL
#define GRAPHICS_COMM_SIZE 0x1000UL

static inline bool is_graphics_comm_range(void* addr, size_t size) {
    uintptr_t start = (uintptr_t)addr;
    return size && start >= GRAPHICS_COMM_ADDR && size <= GRAPHICS_COMM_SIZE
           && start - GRAPHICS_COMM_ADDR <= GRAPHICS_COMM_SIZE - size;
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
#ifdef __x86_64__
    /* the comm page too, so that the LibOS treats it like the arenas (a forked child inherits the
     * identity mapping instead of a copy) and never places other memory there */
    ret = callback(GRAPHICS_COMM_ADDR, GRAPHICS_COMM_SIZE, /*is_user_reserved=*/true,
                   "egl_redirect_comm");
    if (ret < 0)
        return -PAL_ERROR_NOMEM;
#endif
    return 0;
}

/* 1: the base of a graphics arena, mapped whole (the arenas are reserved for libgraphics and
 * mapped as one fixed-size chunk each); 0: not an arena base; -PAL_ERROR_NOMEM: an arena base
 * with another size, an application's MAP_FIXED over the reservation, refused (Wine reserves a
 * PE image's preferred base, 0x140000000 inside the default hugepage arena, this way and copes
 * with the failure; the LibOS refuses such requests first, libos_mmap.c). */
static inline int is_egl_redirect_data_region_base(void* addr, size_t size) {
    if (addr == DATA_REGION || addr == HUGEPAGE_DATA_REGION) {
        size_t expected_size = addr == DATA_REGION ? DATA_REGION_SIZE : HUGEPAGE_REGION_SIZE;
        if (size != expected_size) {
            log_warning("Mapping of %lu bytes at the graphics arena base %p refused (the arena is "
                        "%lu bytes, reserved)", (unsigned long)size, addr,
                        (unsigned long)expected_size);
            return -PAL_ERROR_NOMEM;
        }
        if (!IS_ALIGNED_POW2((uintptr_t)addr, PAGE_SIZE) || !IS_ALIGNED_POW2(size, PAGE_SIZE)) {
            log_error("Unexpected alignment for special base %p (size %lu)", addr,
                      (unsigned long)size);
            BUG();
        }
        return 1;
    }
    return 0;
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

#ifdef PAL_HOST_SEV
    /* SEV: memory that the launch did not write reads as garbage, not zero; the partly used table
     * pages (PDP, PML4) must not keep stale "present" entries */
    sev_memory_before_page_tables(page_tables_addr, total_tables_cnt * PAGE_SIZE);
    _real_memset((void*)page_tables_addr, 0, total_tables_cnt * PAGE_SIZE);
    tree_flags |= g_sev_c_bit_mask;
#endif

    /* page tables with PTE leaf entries */
    uint64_t page_tables_base = ptr;
    for (size_t i = 0; i < pages_4k_cnt; i++) {
        uint64_t entry = ((ptr - page_tables_base) / 8) * PAGE_SIZE + leaf_flags;
#ifdef PAL_HOST_SEV
        entry |= sev_c_bit_for_phys(entry & ~(PAGE_SIZE - 1));
#endif
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
#ifdef PAL_HOST_SEV
    /* SNP: RAM not in the launch image must be validated before use (the final tables below) */
    sev_memory_after_temp_tables();
#endif

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
#ifdef PAL_HOST_SEV
    sev_memory_after_final_tables((uint64_t)memory_address_end,
                                  PAGE_TABLES_ADDR + total_tables_cnt * PAGE_SIZE);
#endif

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
    // log_always("(PAL) Physical Memory Range (%p -- %p)", g_memory_address_start, g_memory_address_end);

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

    int egl_redirect_region = is_egl_redirect_data_region_base(addr, size);
    if (egl_redirect_region < 0)
        return egl_redirect_region;

    if (!read && !write && !execute) {
#ifdef PAL_HOST_SEV
        /* SEV: memory the guest has not written yet reads as ciphertext, not zero, and a later
         * protect() makes these pages accessible without zeroing them (glibc maps a thread stack
         * PROT_NONE and then mprotect()s it), so zero them now (SNP: validate them first) */
        if (!egl_redirect_region) {
            /* PVALIDATE needs the pages mapped */
            int sev_ret = memory_mark_pages_on((uint64_t)addr, size, /*write=*/true,
                                               /*execute=*/false, /*usermode=*/false);
            if (sev_ret < 0)
                return sev_ret;
            sev_ret = sev_validate_on_demand((uint64_t)addr, size);
            if (sev_ret < 0)
                return sev_ret;
            memset(memory_frame_pointer((uintptr_t)addr), 0, size);
        }
#endif
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
#ifdef PAL_HOST_SEV
    /* SNP: private RAM is validated when it is first handed out (PVALIDATE needs it mapped) */
    ret = sev_validate_on_demand((uint64_t)addr, size);
    if (ret < 0)
        return ret;
#endif

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
    if (egl_redirect_region) {
        log_always("Reserved GEM arena %p (size %lu); fresh memfd pages, allocations marked WC on demand",
                   addr, (unsigned long)size);
    } else {
#ifdef __x86_64__
        /* through the supervisor alias when it exists (multiprocess): the current root may be a
         * process root that does not map this identity range (yet) */
        memset(memory_frame_pointer((uintptr_t)addr), 0, size);
#else
        memset(addr, 0, size);
#endif
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
    int egl_redirect_region = is_egl_redirect_data_region_base(addr, size);
    if (egl_redirect_region < 0)
        return egl_redirect_region;
    if (egl_redirect_region || is_graphics_comm_range(addr, size)
            || (space && space->identity_memory)) {
        /* The comm page and the GEM arenas are fixed physical ranges that every process of the
         * guest maps at the same address and shares with the host listener (one graphics session
         * per guest, design-gpu.md): the first process to map one allocates it (the zero-fill of
         * the comm page, the page attributes); a later process only gets the boot root's entries,
         * or its mmap would wipe the session's state and reset the arenas' write-combining. */
        static uintptr_t g_shared_graphics_bases[3];
        bool shared_range = egl_redirect_region || is_graphics_comm_range(addr, size);
        bool already_mapped = false;
        if (shared_range) {
            size_t free_slot = ARRAY_SIZE(g_shared_graphics_bases);
            for (size_t i = 0; i < ARRAY_SIZE(g_shared_graphics_bases); i++) {
                if (g_shared_graphics_bases[i] == (uintptr_t)addr)
                    already_mapped = true;
                else if (!g_shared_graphics_bases[i] && free_slot == ARRAY_SIZE(g_shared_graphics_bases))
                    free_slot = i;
            }
            if (!already_mapped && free_slot < ARRAY_SIZE(g_shared_graphics_bases))
                g_shared_graphics_bases[free_slot] = (uintptr_t)addr;
        }
        if (!already_mapped) {
            int ret = memory_alloc(addr, size, read, write, execute);
            if (ret < 0)
                return ret;
        }
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
        /* An inaccessible reservation has no backing: a later mprotect() gives it fresh frames
         * (keeping the identity frame here would share it with every other process). A pool frame
         * of a previous mapping stays in the non-present entry until after the flush. */
        for (uintptr_t va = (uintptr_t)addr; va < (uintptr_t)addr + size;) {
            uint64_t* entry;
            uintptr_t next;
            int ret = address_space_find_leaf_entry(space, va, &entry, &next);
            if (ret < 0)
                return ret;
            if (ret == 1) {
                va = next; /* no table: nothing is mapped there */
                continue;
            }
            uint64_t physical = *entry & 0x00000ffffffff000UL;
            ret = address_space_restore_pte(space, va, is_frame_pool_frame(physical) ? physical : 0);
            if (ret < 0)
                return ret;
            va += PAGE_SIZE;
        }
        int ret = memory_address_space_flush_range((uintptr_t)addr, size);
        if (ret < 0)
            return ret;
        for (uintptr_t va = (uintptr_t)addr; va < (uintptr_t)addr + size;) {
            uint64_t* entry;
            uintptr_t next;
            ret = address_space_find_leaf_entry(space, va, &entry, &next);
            if (ret < 0)
                return ret;
            if (ret == 1) {
                va = next;
                continue;
            }
            memory_frame_put(*entry & 0x00000ffffffff000UL);
            *entry = 0;
            va += PAGE_SIZE;
        }
#ifdef ASAN
        asan_poison_region((uintptr_t)addr, size, ASAN_POISON_USER);
#endif
        return 0;
    }

    /* Demand-zero paging: install lazy entries; memory_address_space_handle_fault() gives each
     * page a zeroed frame on first access, so a process pays only for the pages it touches. */
    uint64_t lazy = PTE_LAZY | (write ? PTE_WRITE : 0) | (execute ? 0 : PTE_NX);
    for (uintptr_t va = (uintptr_t)addr; va < (uintptr_t)addr + size; va += PAGE_SIZE) {
        /* create all page tables first, so that a failure changes no mapping */
        uint64_t* entry;
        int ret = address_space_get_leaf_entry(space, va, /*make_private=*/true, &entry);
        if (ret < 0)
            return ret;
    }
    bool replaced_frames = false;
    for (uintptr_t va = (uintptr_t)addr; va < (uintptr_t)addr + size; va += PAGE_SIZE) {
        uint64_t* entry;
        int ret = address_space_get_leaf_entry(space, va, /*make_private=*/true, &entry);
        if (ret < 0)
            return ret;
        if (leaf_holds_reference(va, *entry)) {
            /* keep the frame in a non-present entry until other vCPUs dropped it */
            *entry &= ~PTE_PRESENT;
            replaced_frames = true;
        } else {
            *entry = lazy;
        }
        uint64_t current_root;
        __asm__ volatile("mov %%cr3, %0" : "=r"(current_root));
        if ((current_root & 0x00000ffffffff000UL) == space->root_phys)
            __asm__ volatile("invlpg (%0)" : : "r"(va) : "memory");
    }

    int ret = memory_address_space_flush_range((uintptr_t)addr, size);
    if (ret < 0)
        return ret;

    if (replaced_frames) {
        for (uintptr_t va = (uintptr_t)addr; va < (uintptr_t)addr + size; va += PAGE_SIZE) {
            uint64_t* entry;
            ret = address_space_get_leaf_entry(space, va, /*make_private=*/false, &entry);
            if (ret < 0)
                return ret;
            if (leaf_holds_reference(va, *entry)) {
                memory_frame_put(*entry & 0x00000ffffffff000UL);
                *entry = lazy;
            }
        }
    }

#ifdef ASAN
    asan_unpoison_region((uintptr_t)addr, size);
#endif
    return 0;
}

/* Page-fault hook: materializes a demand-zero page of the current address space. Returns false
 * for any other fault. Runs in interrupt context, for faults from user and kernel mode alike. */
bool memory_address_space_handle_fault(uintptr_t addr, uint64_t error_code) {
    const uint64_t addr_mask = 0x00000ffffffff000UL;
    if (!g_frame_pool_bitmap || addr >= MEMORY_ADDRESS_SPACE_LIMIT)
        return false; /* not a user address */
    if ((error_code & PTE_PRESENT) && !(error_code & PTE_WRITE))
        return false; /* a protection violation other than a write: a genuine fault */

    uint64_t root;
    __asm__ volatile("mov %%cr3, %0" : "=r"(root));
    uint64_t* table = memory_frame_pointer(root & addr_mask);
    for (unsigned int shift = 39; shift > 12; shift -= 9) {
        uint64_t entry = table[(addr >> shift) & 0x1ff];
        if (!(entry & PTE_PRESENT) || (entry & PTE_HUGE))
            return false;
        table = memory_frame_pointer(entry & addr_mask);
    }
    uint64_t* pte = &table[(addr >> 12) & 0x1ff];
    uint64_t old = __atomic_load_n(pte, __ATOMIC_ACQUIRE);
    if (old & PTE_PRESENT) {
        if (old & PTE_WRITE)
            return true; /* another vCPU materialized or copied it meanwhile */
        if (!(error_code & PTE_WRITE) || !(old & PTE_COW_WRITABLE))
            return false; /* a write to a page that is read-only for good: a genuine fault */
        /* copy-on-write: the page maps a frame of a file's cache (a MAP_PRIVATE file mapping
         * that allows writes); give this address space its own copy. Only this vCPU's TLB is
         * invalidated here (a shootdown waits for the other vCPUs, which an interrupt handler
         * cannot do): another vCPU of the process keeps a read-only translation of the old
         * frame until its next flush and reads the file's content instead of this write there
         * meanwhile; a write there faults and finds the copy. */
        uintptr_t old_frame = old & addr_mask;
        uintptr_t copy;
        if (cow_copy_frame(old_frame, &copy) < 0) {
            log_error("Out of frames for a copy-on-write page at 0x%lx", addr);
            return false;
        }
        uint64_t new = copy | PTE_PRESENT | PTE_WRITE | PTE_USER | (old & PTE_NX) | PTE_ENC;
        if (!__atomic_compare_exchange_n(pte, &old, new, /*weak=*/false, __ATOMIC_ACQ_REL,
                                         __ATOMIC_ACQUIRE)) {
            memory_frame_free(copy); /* lost a race; the winner's mapping (or change) stands */
            return true;
        }
        __asm__ volatile("invlpg (%0)" : : "r"(addr) : "memory");
        memory_frame_put(old_frame); /* the leaf's reference to the shared frame */
        return true;
    }
    if (error_code & PTE_PRESENT)
        return true; /* the entry changed under the fault (unmapped meanwhile): let it retry */
    if (!(old & PTE_LAZY))
        return false;
    if ((error_code & PTE_WRITE) && !(old & PTE_WRITE))
        return false; /* write to a read-only page: a genuine fault */

    uintptr_t frame;
    if (memory_frame_alloc(&frame) < 0) {
        log_error("Out of frames for a demand-zero page at 0x%lx", addr);
        return false;
    }
    uint64_t new = frame | PTE_PRESENT | (old & PTE_WRITE) | PTE_USER | (old & PTE_NX) | PTE_ENC;
    if (!__atomic_compare_exchange_n(pte, &old, new, /*weak=*/false, __ATOMIC_ACQ_REL,
                                     __ATOMIC_ACQUIRE)) {
        memory_frame_free(frame); /* lost a race; the winner's mapping (or change) stands */
    }
    return true;
}

int memory_address_space_translate(uintptr_t addr, uintptr_t* out_phys) {
    const uint64_t addr_mask = 0x00000ffffffff000UL;
    if (!g_frame_pool_bitmap || addr >= MEMORY_ADDRESS_SPACE_LIMIT)
        return -PAL_ERROR_INVAL;

    uint64_t root;
    __asm__ volatile("mov %%cr3, %0" : "=r"(root));
    uint64_t* table = memory_frame_pointer(root & addr_mask);
    for (unsigned int shift = 39; shift >= 12; shift -= 9) {
        uint64_t entry = __atomic_load_n(&table[(addr >> shift) & 0x1ff], __ATOMIC_ACQUIRE);
        if (!(entry & PTE_PRESENT))
            return -PAL_ERROR_INVAL;
        if (shift == 12 || ((entry & PTE_HUGE) && shift <= 30)) {
            uintptr_t page_mask = (1UL << shift) - 1;
            *out_phys = (entry & addr_mask & ~page_mask) | (addr & page_mask);
            return 0;
        }
        table = memory_frame_pointer(entry & addr_mask);
    }
    return -PAL_ERROR_INVAL;
}

__attribute_no_sanitize_address
int memory_address_space_free(struct memory_address_space* space, void* addr, size_t size) {
    if (!g_frame_pool_bitmap)
        return memory_free(addr, size);
    if (is_graphics_comm_range(addr, size) && !(space && space->identity_memory)) {
        /* the boot root keeps the comm page: PAL helper threads (the vsock driver's input ring)
         * read it there at any time */
        return memory_address_space_share_range(memory_address_space_boot(), space,
                                                (uintptr_t)addr, size);
    }
    if (is_egl_redirect_data_region_range(addr, size) || (space && space->identity_memory)) {
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
    for (uintptr_t va = (uintptr_t)addr; va < (uintptr_t)addr + size;) {
        uint64_t* entry;
        uintptr_t next;
        int ret = address_space_find_leaf_entry(space, va, &entry, &next);
        if (ret < 0)
            return ret;
        if (ret == 1) {
            va = next; /* no table: nothing is mapped there */
            continue;
        }
        uintptr_t frame = *entry & addr_mask;
        ret = address_space_get_leaf_entry(space, va, /*make_private=*/true, &entry);
        if (ret < 0)
            return ret;
        *entry = frame;

        uint64_t current_root;
        __asm__ volatile("mov %%cr3, %0" : "=r"(current_root));
        if ((current_root & addr_mask) == space->root_phys)
            __asm__ volatile("invlpg (%0)" : : "r"(va) : "memory");
        va += PAGE_SIZE;
    }
    int ret = memory_address_space_flush_range((uintptr_t)addr, size);
    if (ret < 0)
        return ret;

    /* Do not return a physical frame to the pool until remote vCPUs have dropped cached mappings
     * to it. The not-present PTEs keep the frame addresses available for this second pass. */
    for (uintptr_t va = (uintptr_t)addr; va < (uintptr_t)addr + size;) {
        uint64_t* entry;
        uintptr_t next;
        ret = address_space_find_leaf_entry(space, va, &entry, &next);
        if (ret < 0)
            return ret;
        if (ret == 1) {
            va = next;
            continue;
        }
        uintptr_t frame = *entry & addr_mask;
        if (is_frame_pool_frame(frame)) {
            memory_frame_put(frame);
            *entry = 0;
        }
        va += PAGE_SIZE;
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
    if (is_egl_redirect_data_region_range(addr, size) || is_graphics_comm_range(addr, size)
            || (space && space->identity_memory)) {
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
    uint64_t lazy = (!read && !write && !execute)
                    ? 0 : PTE_LAZY | (write ? PTE_WRITE : 0) | (execute ? 0 : PTE_NX);
    for (uintptr_t va = (uintptr_t)addr; va < (uintptr_t)addr + size; va += PAGE_SIZE) {
        uint64_t* entry;
        uintptr_t next;
        int ret = address_space_find_leaf_entry(space, va, &entry, &next);
        if (ret < 0)
            return ret;
        if (ret == 1) {
            /* no table yet (a reservation beyond the boot tables): an unbacked page; a demand-zero
             * entry is installed if it is to become accessible, otherwise there is nothing to do
             * up to the missing table's reach */
            if (!lazy) {
                va = MIN(next, (uintptr_t)addr + size) - PAGE_SIZE;
                continue;
            }
            ret = address_space_restore_pte(space, va, lazy);
            if (ret < 0)
                return ret;
            continue;
        }
        uintptr_t frame = *entry & addr_mask;
        uint64_t cow = *entry & (PTE_COW | PTE_COW_WRITABLE);
        if (!(*entry & PTE_PRESENT) && !leaf_holds_reference(va, *entry)) {
            /* an unbacked page (never touched, or a PROT_NONE reservation) stays demand-zero */
            ret = address_space_restore_pte(space, va, lazy);
        } else if (!read && !write && !execute) {
            ret = address_space_restore_pte(space, va, frame | (cow & PTE_COW) | PTE_ENC);
        } else if (cow && write) {
            /* a copy-on-write page (a frame of a file's cache) becomes writable: copy it now;
             * map_owned drops the leaf's reference to the shared frame */
            uintptr_t copy;
            ret = cow_copy_frame(frame, &copy);
            if (ret < 0)
                return ret;
            ret = memory_address_space_map_owned(space, va, copy, write, execute,
                                                 /*usermode=*/true);
            if (ret < 0)
                memory_frame_free(copy);
        } else {
            ret = memory_address_space_map(space, va, frame, write, execute, /*usermode=*/true);
            if (ret == 0 && cow) {
                /* still a shared frame, read-only: keep the mark (a later write must copy; it
                 * is allowed by a fault only while the mapping is writable, which it is not) */
                uint64_t* leaf;
                ret = address_space_get_leaf_entry(space, va, /*make_private=*/true, &leaf);
                if (ret == 0)
                    *leaf |= PTE_COW;
            }
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

/* MADV_DONTNEED for an anonymous range of a demand-paged process: every page that holds a frame
 * goes back to a demand-zero entry and its frame to the pool, pages that were never touched stay
 * as they are (they cost nothing, and filling them with zeros would allocate a frame for each).
 * Returns -PAL_ERROR_NOTIMPLEMENTED without changing anything if a page of the range is not plain
 * private anonymous memory of this space (shared or copy-on-write frames, device memory, the
 * graphics ranges): the caller then zeroes the range by hand. */
__attribute_no_sanitize_address
int memory_address_space_discard(struct memory_address_space* space, void* addr, size_t size) {
    const uint64_t addr_mask = 0x00000ffffffff000UL;
    if (!g_frame_pool_bitmap || !space || !space->root || space->identity_memory ||
            is_egl_redirect_data_region_range(addr, size) || is_graphics_comm_range(addr, size))
        return -PAL_ERROR_NOTIMPLEMENTED;
    if (!IS_ALIGNED_POW2((uintptr_t)addr, PAGE_SIZE) || !size ||
            !IS_ALIGNED_POW2(size, PAGE_SIZE) || (uintptr_t)addr + size < (uintptr_t)addr)
        return -PAL_ERROR_INVAL;
    if ((uintptr_t)addr < SHARED_MEM_ADDR + SHARED_MEM_SIZE &&
            SHARED_MEM_ADDR < (uintptr_t)addr + size)
        return -PAL_ERROR_DENIED;

    const uintptr_t end = (uintptr_t)addr + size;

    /* first pass: nothing is changed unless every page is of a kind handled below */
    for (uintptr_t va = (uintptr_t)addr; va < end;) {
        uint64_t* entry;
        uintptr_t next;
        int ret = address_space_find_leaf_entry(space, va, &entry, &next);
        if (ret < 0)
            return -PAL_ERROR_NOTIMPLEMENTED; /* a huge page */
        if (ret == 1) {
            va = next; /* no table: nothing is mapped there */
            continue;
        }
        uint64_t e = __atomic_load_n(entry, __ATOMIC_ACQUIRE);
        if ((e & (PTE_COW | PTE_COW_WRITABLE)) || ((e & PTE_PRESENT) && !leaf_holds_reference(va, e)))
            return -PAL_ERROR_NOTIMPLEMENTED;
        va += PAGE_SIZE;
    }

    /* second pass, in batches: an entry that holds a frame is switched to demand-zero in one
     * atomic step (a thread that touches the page meanwhile either still reaches the old frame or
     * faults in a fresh zero page, never sees a hole); the frames are returned to the pool only
     * after the other vCPUs have dropped their translations */
    enum { BATCH = 256 };
    uintptr_t frames[BATCH];
    size_t nframes = 0;
    uintptr_t batch_begin = 0;
    for (uintptr_t va = (uintptr_t)addr; va <= end;) {
        bool flush = false;
        if (va < end) {
            uint64_t* entry;
            uintptr_t next;
            int ret = address_space_find_leaf_entry(space, va, &entry, &next);
            if (ret < 0)
                return ret;
            if (ret == 1) {
                va = next;
                continue;
            }
            ret = address_space_get_leaf_entry(space, va, /*make_private=*/true, &entry);
            if (ret < 0)
                return ret;
            uint64_t e = __atomic_load_n(entry, __ATOMIC_ACQUIRE);
            while (leaf_holds_reference(va, e)) {
                /* present: back to demand-zero with the same rights; a PROT_NONE page (the frame
                 * kept in a not-present entry) back to an empty entry */
                uint64_t fresh = (e & PTE_PRESENT) ? (PTE_LAZY | (e & PTE_WRITE) | (e & PTE_NX)) : 0;
                if (__atomic_compare_exchange_n(entry, &e, fresh, /*weak=*/false, __ATOMIC_ACQ_REL,
                                                __ATOMIC_ACQUIRE)) {
                    if (!nframes)
                        batch_begin = va;
                    frames[nframes++] = e & addr_mask;
                    uint64_t current_root;
                    __asm__ volatile("mov %%cr3, %0" : "=r"(current_root));
                    if ((current_root & addr_mask) == space->root_phys)
                        __asm__ volatile("invlpg (%0)" : : "r"(va) : "memory");
                    break;
                }
            }
            va += PAGE_SIZE;
            flush = nframes == BATCH;
        } else {
            flush = true;
        }
        if (flush) {
            if (nframes) {
                uintptr_t batch_end = (va < end ? va : end);
                int ret = memory_address_space_flush_range(batch_begin, batch_end - batch_begin);
                if (ret < 0)
                    return ret;
                for (size_t i = 0; i < nframes; i++)
                    memory_frame_put(frames[i]);
                nframes = 0;
            }
            if (va >= end)
                break;
        }
    }
#ifdef ASAN
    asan_unpoison_region((uintptr_t)addr, size);
#endif
    return 0;
}
#endif
