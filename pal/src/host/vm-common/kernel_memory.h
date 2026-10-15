/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2023 Intel Corporation */

/* Declarations for memory-specific operations. */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "spinlock.h"

#define PAGE_TABLES_ADDR 0x20000000UL          /* page tables occupy [512MB, 658MB) */
#define PAGE_TABLES_SIZE (146UL * 1024 * 1024) /* 146MB, enough to describe 64GB of addr space +
                                                * AddressSanitizer shadow memory for these 64GB */

#define SHARED_MEM_ADDR  0x29200000UL          /* shared memory occupies [658MB, 896MB) */
#define SHARED_MEM_SIZE  (238UL * 1024 * 1024) /* 238MB */

#define PCI_HOLE_ADDR    0x80000000UL          /* QEMU's memory hole + PCI (BARs, LAPIC, IOAPIC) */
#define PCI_HOLE_SIZE    0x80000000UL          /* 2GB */

/* equivalent to E820_TABLE_ENTRY in EFI_HOB_E820_TABLE (needs to be packed) */
#define E820_ADDRESS_RANGE_MEMORY   1
#define E820_ADDRESS_RANGE_RESERVED 2
typedef struct {
    uint64_t  address;
    uint64_t  size;
    uint32_t  type;
} __attribute__((packed)) e820_table_entry;

/* max number of entries in the E820 table; taken from QEMU sources */
#define E820_NR_ENTRIES 16

#define E820_TABLE_MAX_SIZE (E820_NR_ENTRIES * sizeof(e820_table_entry))

extern uint64_t g_pml4_table_base;

void* memory_get_shared_region(size_t size);
int memory_free_shared_region(void* addr, size_t size);

int memory_find_page_table_entry(uint64_t addr, uint64_t** out_pte_addr);
int memory_mark_pages_on(uint64_t addr, size_t size, bool write, bool execute, bool usermode);
int memory_mark_pages_off(uint64_t addr, size_t size);
int memory_mark_pages_strong_uncacheable(uint64_t addr, size_t size, bool mark);
int memory_mark_pages_write_combined(uint64_t addr, size_t size, bool mark);

int memory_pagetables_init(void* memory_address_end, bool current_page_tables_cover_1gb);

#ifdef __x86_64__
/* Reserve a supervisor-only range for private process frames. `addr` and `size` must be page
 * aligned and the range must not overlap memory already reserved for another PAL use. */
int memory_frame_pool_init(uintptr_t addr, size_t size);
int memory_frame_alloc(uintptr_t* out_frame);
int memory_frame_free(uintptr_t frame);
int memory_frame_alloc_contiguous(size_t size, void** out_alias);
int memory_frame_free_contiguous(void* alias, size_t size);
bool memory_frame_is_alias_pointer(const void* pointer);
size_t memory_frame_allocated_count(void);
/* True once the experimental multiprocess support has set up the frame pool. */
bool memory_frame_pool_active(void);
/* Return the stable supervisor alias for a physical frame address. */
void* memory_frame_pointer(uintptr_t frame);

struct memory_address_space {
    uint64_t* root;
    uintptr_t root_phys;
    uint64_t* base_root;
    uintptr_t owned_frame_head;
    uintptr_t owner_frame;
    /* set when the process exits: its remaining threads (except `exit_thread`, which performs
     * the exit) are never scheduled again and are removed before the address space is destroyed */
    uint32_t exiting;
    void* exit_thread;
    /* the application memory of this space is the identity-mapped guest RAM of the boot layout,
     * as without processes: set for the first process when the graphics redirection is
     * configured, whose host side reads guest pointers as guest-physical addresses */
    bool identity_memory;
    /* PAL handles this process obtained through the PAL API and has not destroyed; the process's
     * exit destroys them (see vm_track_handle()); array in PAL memory, guarded by the lock */
    void** handles;
    size_t handles_count;
    size_t handles_cap;
    spinlock_t handles_lock;
};
struct memory_address_space* memory_address_space_boot(void);
int memory_address_space_clone(uint64_t base_root, struct memory_address_space* out_space);
int memory_address_space_clone_from(const struct memory_address_space* source,
                                    struct memory_address_space* out_space);
size_t memory_address_space_destroy(struct memory_address_space* space);
int memory_address_space_copy_private_range(struct memory_address_space* source,
                                            struct memory_address_space* target, uintptr_t addr,
                                            size_t size);
int memory_address_space_map(struct memory_address_space* space, uintptr_t addr,
                             uintptr_t frame, bool write, bool execute, bool usermode);
/* Maps `frame` at `addr` and hands the caller's reference on it to the leaf (the address space
 * releases it when the leaf is replaced or the space is destroyed); releases the replaced leaf's
 * frame. memory_address_space_map() is the raw variant that moves no references. */
int memory_address_space_map_owned(struct memory_address_space* space, uintptr_t addr,
                                   uintptr_t frame, bool write, bool execute, bool usermode);
/* Maps a frame that stays referenced elsewhere too (a shared file page): the leaf takes its own
 * reference. */
int memory_address_space_map_shared_frame(struct memory_address_space* space, uintptr_t addr,
                                          uintptr_t frame, bool write, bool execute);
int memory_address_space_share_range(struct memory_address_space* source,
                                     struct memory_address_space* target, uintptr_t addr,
                                     size_t size);
int memory_address_space_share_range_noflush(struct memory_address_space* source,
                                            struct memory_address_space* target,
                                            uintptr_t addr, size_t size, bool* out_changed);
int memory_address_space_flush_range(uintptr_t addr, size_t size);
int memory_address_space_alloc(struct memory_address_space* space, void* addr, size_t size,
                               bool read, bool write, bool execute);
int memory_address_space_free(struct memory_address_space* space, void* addr, size_t size);
int memory_address_space_protect(struct memory_address_space* space, void* addr, size_t size,
                                 bool read, bool write, bool execute);
int memory_address_space_switch(const struct memory_address_space* space);
bool memory_address_space_handle_fault(uintptr_t addr, uint64_t error_code);
int memory_address_space_switch_root(uint64_t root);
#endif

/* Ensures IA32_PAT has Write-Combining at index 1 (fatal on failure). */
void memory_pat_init_for_wc(void);
int memory_preload_ranges(e820_table_entry* e820_entries, size_t e820_entries_size,
                          int (*callback)(uintptr_t addr, size_t size, bool is_user_reserved,
                                          const char* comment));

/* Graphics GEM arenas: applies the geometry (data-arena address *and* size -- a zero address
 * means the compiled 4G anchor; hugepage arena address, computed when not supplied) and
 * reserves both ranges through `callback`. Call before any boot-time allocator activity
 * (vm/pal_main.c does this right after the e820 pass, from fw_cfg input). */
void graphics_arenas_set_geometry(uintptr_t data_addr, size_t data_size, uintptr_t huge_addr);
int memory_reserve_graphics_arenas(int (*callback)(uintptr_t addr, size_t size,
                                                   bool is_user_reserved,
                                                   const char* comment));
int memory_tighten_permissions(void);

int memory_alloc(void* addr, size_t size, bool read, bool write, bool execute);
int memory_protect(void* addr, size_t size, bool read, bool write, bool execute);
int memory_free(void* addr, size_t size);

int memory_init(e820_table_entry* e820_entries, size_t e820_entries_size,
                void** out_memory_address_start, void** out_memory_address_end);
