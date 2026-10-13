/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

#include <stdbool.h>
#include <stdint.h>

#include "libos_context.h"
#include "libos_entry.h"
#include "libos_flags_conv.h"
#include "libos_handle.h"
#include "libos_internal.h"
#include "libos_utils.h"
#include "libos_vma.h"
#include "linux_abi/memory.h"
#include "pal.h"
#include "spinlock.h"
#include "toml_utils.h"

#define ECALL_INSN      0x00000073u
#define STUB_SIZE       56
#define STUB_PAGE_SIZE  4096
#define JAL_RANGE       (1ull << 20)
#define MAX_STUB_PAGES  512

static const unsigned g_stub_saved_regs[] = {5, 6, 7, 28, 29}; /* t0, t1, t2, t3, t4 */

struct stub_page { uintptr_t base; unsigned used; };
static spinlock_t g_patch_lock = INIT_SPINLOCK_UNLOCKED;
static struct stub_page g_stub_pages[MAX_STUB_PAGES];
static size_t g_stub_pages_cnt;
static int g_enabled = -1;
static size_t g_patched_sites;

extern void libos_syscall_entry_patched(void);

static bool patching_enabled(void) {
    int e = __atomic_load_n(&g_enabled, __ATOMIC_ACQUIRE);
    if (e >= 0)
        return e;
    bool val = true;
    if (toml_bool_in(g_manifest_root, "sys.rewrite_syscalls", true, &val) < 0)
        val = false;
    __atomic_store_n(&g_enabled, val, __ATOMIC_RELEASE);
    return val;
}

static bool flush_icache(void* begin, size_t size) {
    __asm__ volatile("fence.i" ::: "memory");
    if (pal_arch_flush_icache(begin, size) < 0) {
        log_debug("syscall-patch: riscv_flush_icache failed");
        return false;
    }
    return true;
}

static uint32_t enc_i(int imm, unsigned rs1, unsigned funct3, unsigned rd, unsigned opcode) {
    return ((uint32_t)imm & 0xfff) << 20 | rs1 << 15 | funct3 << 12 | rd << 7 | opcode;
}
static uint32_t enc_s(int imm, unsigned rs1, unsigned rs2) {
    return (((uint32_t)imm >> 5) & 0x7f) << 25 | rs2 << 20 | rs1 << 15 | 3 << 12 |
           ((uint32_t)imm & 0x1f) << 7 | 0x23;
}
static uint32_t enc_jal(int64_t delta) {
    uint32_t d = (uint32_t)delta;
    return 0x6fu | ((d >> 20) & 1) << 31 | ((d >> 1) & 0x3ff) << 21 |
           ((d >> 11) & 1) << 20 | ((d >> 12) & 0xff) << 12;
}

static uint32_t load_insn(uintptr_t pc) {
    const volatile uint16_t* halves = (const volatile uint16_t*)pc;
    return halves[0] | (uint32_t)halves[1] << 16;
}

static uint32_t* alloc_stub(uintptr_t site) {
    /* A published page must remain executable: changing a shared page back to RW while adding a
     * later stub lets another hart fault while executing an older stub. Anonymous PAL mappings
     * have no writable alias, so use one immutable page per site: allocate RW, populate it, then
     * switch it to RX exactly once before publishing the site's jal. */
    if (g_stub_pages_cnt == MAX_STUB_PAGES)
        return NULL;
    uintptr_t lo = site > JAL_RANGE ? site - JAL_RANGE : 0;
    uintptr_t hi = site + JAL_RANGE;
    if (lo < (uintptr_t)g_pal_public_state->memory_address_start)
        lo = (uintptr_t)g_pal_public_state->memory_address_start;
    if (hi > (uintptr_t)g_pal_public_state->memory_address_end)
        hi = (uintptr_t)g_pal_public_state->memory_address_end;
    lo = (lo + STUB_PAGE_SIZE - 1) & ~(uintptr_t)(STUB_PAGE_SIZE - 1);
    hi &= ~(uintptr_t)(STUB_PAGE_SIZE - 1);
    void* addr = NULL;
    if (hi <= lo + STUB_PAGE_SIZE)
        return NULL;
    if (bkeep_mmap_any_in_range((void*)lo, (void*)hi, STUB_PAGE_SIZE,
                                PROT_READ | PROT_EXEC,
                                MAP_PRIVATE | MAP_ANONYMOUS | VMA_INTERNAL, NULL, 0,
                                "syscall-stubs", &addr) < 0)
        return NULL;
    if (PalVirtualMemoryAlloc(addr, STUB_PAGE_SIZE, PAL_PROT_READ | PAL_PROT_WRITE) < 0) {
        void* tmp_vma = NULL;
        if (bkeep_munmap(addr, STUB_PAGE_SIZE, /*is_internal=*/true, &tmp_vma) < 0)
            BUG();
        bkeep_remove_tmp_vma(tmp_vma);
        return NULL;
    }
    g_stub_pages[g_stub_pages_cnt] = (struct stub_page){.base = (uintptr_t)addr, .used = 1};
    __atomic_store_n(&g_stub_pages_cnt, g_stub_pages_cnt + 1, __ATOMIC_RELEASE);
    return addr;
}

bool libos_try_patch_syscall_site(uintptr_t pc) {
    if (!patching_enabled() || (pc & 1))
        return false;
    struct libos_vma_info vma;
    if (lookup_vma((void*)pc, &vma) < 0)
        return false;
    bool eligible = vma.file && (vma.prot & PROT_EXEC) && !(vma.prot & PROT_WRITE) &&
                    (vma.flags & MAP_PRIVATE) && !(vma.flags & (VMA_INTERNAL | VMA_UNMAPPED));
    int prot = vma.prot, flags = vma.flags;
    if (vma.file)
        put_handle(vma.file);
    if (!eligible)
        return false;
    spinlock_lock(&g_patch_lock);
    uint32_t* site = (uint32_t*)pc;
    if (load_insn(pc) != ECALL_INSN) {
        spinlock_unlock(&g_patch_lock);
        return false;
    }
    uint32_t* stub = alloc_stub(pc);
    if (!stub) {
        spinlock_unlock(&g_patch_lock);
        return false;
    }
    int64_t delta = (intptr_t)stub - (intptr_t)pc;
    if (delta <= -(1ll << 20) || delta >= (1ll << 20)) {
        spinlock_unlock(&g_patch_lock);
        return false;
    }
    uintptr_t stub_page = (uintptr_t)stub & ~(uintptr_t)(STUB_PAGE_SIZE - 1);
    stub[0] = enc_i(-48, 2, 0, 2, 0x13); /* addi sp,sp,-48 */
    for (size_t i = 0; i < ARRAY_SIZE(g_stub_saved_regs); i++)
        stub[1 + i] = enc_s(i * 8, 2, g_stub_saved_regs[i]); /* sd t0..t4,offset(sp) */
    stub[6] = 0x00000297;                 /* auipc t0,0 */
    stub[7] = enc_i(16, 5, 3, 6, 0x03);   /* ld t1,16(t0): resume */
    stub[8] = enc_i(24, 5, 3, 5, 0x03);   /* ld t0,24(t0): entry */
    stub[9] = 0x00028067;                 /* jalr zero,t0,0 */
    *(uint64_t*)&stub[10] = pc + 4;
    *(uint64_t*)&stub[12] = (uintptr_t)&libos_syscall_entry_patched;
    if (!flush_icache(stub, STUB_SIZE) ||
        PalVirtualMemoryProtect((void*)stub_page, STUB_PAGE_SIZE,
                                PAL_PROT_READ | PAL_PROT_EXEC) < 0) {
        spinlock_unlock(&g_patch_lock);
        return false;
    }
    uintptr_t page = pc & ~(uintptr_t)(STUB_PAGE_SIZE - 1);
    pal_prot_flags_t orig = LINUX_PROT_TO_PAL(prot, flags);
    if (PalVirtualMemoryProtect((void*)page, STUB_PAGE_SIZE,
                                PAL_PROT_READ | PAL_PROT_WRITE | PAL_PROT_EXEC | PAL_PROT_WRITECOPY) < 0) {
        spinlock_unlock(&g_patch_lock);
        return false;
    }
    uint32_t branch = enc_jal(delta);
    if (pc & 3) {
        if (pal_arch_publish_unaligned_insn(site, branch) < 0) {
            if (PalVirtualMemoryProtect((void*)page, STUB_PAGE_SIZE, orig) < 0)
                BUG();
            spinlock_unlock(&g_patch_lock);
            return false;
        }
    } else {
        __atomic_store_n(site, branch, __ATOMIC_RELEASE);
        if (!flush_icache(site, sizeof(*site)))
            BUG();
    }
    if (PalVirtualMemoryProtect((void*)page, STUB_PAGE_SIZE, orig) < 0)
        BUG();
    g_patched_sites++;
    log_debug("syscall-patch: site %#lx rewritten -> stub %p (%zu sites patched)", pc, stub,
              g_patched_sites);
    spinlock_unlock(&g_patch_lock);
    return true;
}

bool libos_syscall_site_was_patched(uintptr_t pc) {
    uint32_t insn = load_insn(pc);
    if ((insn & 0x7f) != 0x6f || ((insn >> 7) & 0x1f) != 0) /* jal x0, offset */
        return false;
    int64_t delta = ((int64_t)(insn >> 31) << 20) |
                    ((int64_t)((insn >> 12) & 0xff) << 12) |
                    ((int64_t)((insn >> 20) & 1) << 11) |
                    ((int64_t)((insn >> 21) & 0x3ff) << 1);
    if (delta & (1 << 20))
        delta -= 1 << 21;
    uintptr_t target = (uintptr_t)((intptr_t)pc + delta);
    size_t count = __atomic_load_n(&g_stub_pages_cnt, __ATOMIC_ACQUIRE);
    for (size_t i = 0; i < count; i++) {
        if (target >= g_stub_pages[i].base && target < g_stub_pages[i].base + STUB_PAGE_SIZE)
            return true;
    }
    return false;
}

bool libos_arch_is_intentional_internal_insn(uintptr_t addr) {
    void* tramp = libos_rt_sigreturn_trampoline_addr();
    return addr >= (uintptr_t)tramp && addr < (uintptr_t)tramp + 16
           && *(const uint32_t*)addr == 0x00000073;
}

bool libos_arch_repair_illegal_insn(PAL_CONTEXT* context) {
    __UNUSED(context);
    return false;
}
