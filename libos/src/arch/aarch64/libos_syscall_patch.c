/* SPDX-License-Identifier: LGPL-3.0-or-later */

/*
 * PROTOTYPE: lazy rewriting of trapped `svc #0` sites (opt-in via the manifest option
 * `sys.rewrite_syscalls = true`).
 *
 * Without a patched guest libc every guest syscall takes the seccomp-trap path (SIGSYS + signal
 * frame + rt_sigreturn, ~1.2 us). The first time a given `svc #0` traps, maybe_emulate_syscall()
 * calls libos_try_patch_syscall_site(), which replaces that one instruction with a `b` to a
 * per-site stub. The stub reproduces exactly what maybe_emulate_syscall() does on the trap path
 * (X16 = resume address, then enter libos_syscall_entry), so later executions of the site skip
 * the kernel trap entirely. The libc is not modified and any libc works (glibc, musl, static).
 *
 * Stub (32 bytes, X16/X17 are the sanctioned IP0/IP1 scratch registers; flags are untouched):
 *     movz x16, #resume[15:0]
 *     movk x16, #resume[31:16], lsl #16
 *     movk x16, #resume[47:32], lsl #32
 *     ldr  x17, 1f
 *     br   x17
 *     nop
 * 1:  .quad libos_syscall_entry
 *
 * Constraints / known limitations of the prototype:
 *  - `b` reaches +-128 MiB, so stubs live in internal VMAs allocated within +-120 MiB of the site.
 *  - Only file-backed, private, non-writable, executable, non-internal VMAs are patched. Such VMAs
 *    are re-mapped from the file (not copied) when a child process is created, so a child simply
 *    starts unpatched and re-patches lazily; stubs never have to be migrated.
 *  - The page is made RWX for the duration of the store, then restored. An application mprotect()
 *    racing on the same page in that window is not handled.
 *  - Replacing SVC by B while other cores may execute it is a permitted concurrent modification of
 *    an instruction per the Arm ARM; we publish with a full cache-maintenance sequence.
 */

#include <stdbool.h>
#include <stdint.h>

#include "api.h"
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

#define SVC0_INSN       0xd4000001u
#define STUB_SIZE       32
#define STUB_PAGE_SIZE  4096
#define STUBS_PER_PAGE  (STUB_PAGE_SIZE / STUB_SIZE)
#define B_RANGE         (120ull << 20) /* a little under the +-128 MiB reach of `b` */
#define MAX_STUB_PAGES  256

struct stub_page {
    uintptr_t base;
    unsigned  used;
};

static spinlock_t g_patch_lock = INIT_SPINLOCK_UNLOCKED;
static struct stub_page g_stub_pages[MAX_STUB_PAGES];
static size_t g_stub_pages_cnt;
static int g_enabled = -1; /* -1 = not read yet */
static size_t g_patched_sites;

static bool patching_enabled(void) {
    int e = __atomic_load_n(&g_enabled, __ATOMIC_ACQUIRE);
    if (e >= 0)
        return e;
    bool val = false;
    int ret = toml_bool_in(g_manifest_root, "sys.rewrite_syscalls", /*defaultval=*/true, &val);
    if (ret < 0) {
        log_error("Cannot parse 'sys.rewrite_syscalls' (the value must be `true` or `false`)");
        val = false;
    }
    __atomic_store_n(&g_enabled, val ? 1 : 0, __ATOMIC_RELEASE);
    return val;
}

static inline uint32_t enc_movz(unsigned rd, uint16_t imm, unsigned shift) {
    return 0xd2800000u | ((shift / 16) << 21) | ((uint32_t)imm << 5) | rd;
}
static inline uint32_t enc_movk(unsigned rd, uint16_t imm, unsigned shift) {
    return 0xf2800000u | ((shift / 16) << 21) | ((uint32_t)imm << 5) | rd;
}

/* Make freshly written instructions visible to instruction fetch on all cores (inner-shareable).
 * libsysdb is linked -nostdlib, so no __clear_cache(); this is the standard sequence. */
static void flush_icache(void* begin, size_t len) {
    uint64_t ctr;
    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
    uintptr_t dline = 4u << ((ctr >> 16) & 0xf);
    uintptr_t iline = 4u << (ctr & 0xf);
    uintptr_t start = (uintptr_t)begin, end = start + len;

    for (uintptr_t a = start & ~(dline - 1); a < end; a += dline)
        __asm__ volatile("dc cvau, %0" ::"r"(a) : "memory");
    __asm__ volatile("dsb ish" ::: "memory");
    for (uintptr_t a = start & ~(iline - 1); a < end; a += iline)
        __asm__ volatile("ic ivau, %0" ::"r"(a) : "memory");
    __asm__ volatile("dsb ish\n\tisb" ::: "memory");
}

/* Returns a free 32-byte slot within `B_RANGE` of `site`, allocating a stub page if needed. Called
 * with g_patch_lock held. */
static uint32_t* alloc_stub(uintptr_t site) {
    for (size_t i = 0; i < g_stub_pages_cnt; i++) {
        struct stub_page* p = &g_stub_pages[i];
        if (p->used >= STUBS_PER_PAGE)
            continue;
        uintptr_t slot = p->base + (uintptr_t)p->used * STUB_SIZE;
        uint64_t dist = slot > site ? slot - site : site - slot;
        if (dist < B_RANGE) {
            p->used++;
            return (uint32_t*)slot;
        }
    }
    if (g_stub_pages_cnt == MAX_STUB_PAGES)
        return NULL;

    uintptr_t lo = site > B_RANGE ? site - B_RANGE : 0;
    uintptr_t hi = site + B_RANGE;
    if (lo < (uintptr_t)g_pal_public_state->memory_address_start)
        lo = (uintptr_t)g_pal_public_state->memory_address_start;
    if (hi > (uintptr_t)g_pal_public_state->memory_address_end)
        hi = (uintptr_t)g_pal_public_state->memory_address_end;
    lo = (lo + STUB_PAGE_SIZE - 1) & ~(uintptr_t)(STUB_PAGE_SIZE - 1);
    hi &= ~(uintptr_t)(STUB_PAGE_SIZE - 1);
    if (hi <= lo + STUB_PAGE_SIZE)
        return NULL;

    void* addr = NULL;
    int prot = PROT_READ | PROT_WRITE | PROT_EXEC;
    int flags = MAP_PRIVATE | MAP_ANONYMOUS | VMA_INTERNAL;
    int ret = bkeep_mmap_any_in_range((void*)lo, (void*)hi, STUB_PAGE_SIZE, prot, flags, NULL, 0,
                                      "syscall-stubs", &addr);
    if (ret < 0) {
        log_debug("syscall-patch: bkeep_mmap_any_in_range [%#lx,%#lx) failed: %d", lo, hi, ret);
        return NULL;
    }
    ret = PalVirtualMemoryAlloc(addr, STUB_PAGE_SIZE,
                                PAL_PROT_READ | PAL_PROT_WRITE | PAL_PROT_EXEC);
    if (ret < 0) {
        log_debug("syscall-patch: PalVirtualMemoryAlloc %p failed: %d", addr, ret);
        void* tmp_vma = NULL;
        if (bkeep_munmap(addr, STUB_PAGE_SIZE, /*is_internal=*/true, &tmp_vma) < 0)
            BUG();
        bkeep_remove_tmp_vma(tmp_vma);
        return NULL;
    }
    struct stub_page* p = &g_stub_pages[g_stub_pages_cnt];
    p->base = (uintptr_t)addr;
    p->used = 1;
    /* Publish for the lock-free reader in libos_syscall_site_was_patched(). */
    __atomic_store_n(&g_stub_pages_cnt, g_stub_pages_cnt + 1, __ATOMIC_RELEASE);
    return (uint32_t*)addr;
}

bool libos_try_patch_syscall_site(uintptr_t pc) {
    if (!patching_enabled() || (pc & 3)) {
        log_debug("syscall-patch: disabled/misaligned (%d)", g_enabled);
        return false;
    }
    /* The stub encodes the resume address in 48 bits (movz + 2x movk). */
    if ((pc + 4) >> 48)
        return false;

    struct libos_vma_info vma;
    if (lookup_vma((void*)pc, &vma) < 0) {
        log_debug("syscall-patch: lookup_vma failed for %#lx", pc);
        return false;
    }
    bool eligible = vma.file && (vma.prot & PROT_EXEC) && !(vma.prot & PROT_WRITE)
                    && (vma.flags & MAP_PRIVATE) && !(vma.flags & (VMA_INTERNAL | VMA_UNMAPPED));
    int vma_prot = vma.prot;
    int vma_flags = vma.flags;
    int vma_file_dbg = vma.file != NULL;
    if (vma.file)
        put_handle(vma.file);
    if (!eligible) {
        log_debug("syscall-patch: ineligible vma file=%d prot=%#x flags=%#x", vma_file_dbg, vma_prot, vma_flags);
        return false;
    }

    bool patched = false;
    spinlock_lock(&g_patch_lock);

    volatile uint32_t* site = (volatile uint32_t*)pc;
    if (*site != SVC0_INSN) {
        /* Another thread already patched this site (or it is not a plain svc anymore). */
        spinlock_unlock(&g_patch_lock);
        return false;
    }

    uint32_t* stub = alloc_stub(pc);
    if (!stub) {
        log_debug("syscall-patch: alloc_stub failed for %#lx", pc);
        goto out;
    }

    int64_t delta = (int64_t)((uintptr_t)stub - pc);
    if (delta <= -(1ll << 27) || delta >= (1ll << 27))
    {
        log_debug("syscall-patch: stub out of range %ld", delta);
        goto out;
    }

    uint64_t resume = pc + 4;
    uint64_t entry = (uint64_t)&libos_syscall_entry;
    stub[0] = enc_movz(16, resume & 0xffff, 0);
    stub[1] = enc_movk(16, (resume >> 16) & 0xffff, 16);
    stub[2] = enc_movk(16, (resume >> 32) & 0xffff, 32);
    stub[3] = 0x58000071; /* ldr x17, #12 -> the literal at stub+24 */
    stub[4] = 0xd61f0220; /* br x17 */
    stub[5] = 0xd503201f; /* nop */
    *(uint64_t*)&stub[6] = entry;
    flush_icache(stub, STUB_SIZE);

    uintptr_t page = pc & ~(uintptr_t)(STUB_PAGE_SIZE - 1);
    pal_prot_flags_t orig = LINUX_PROT_TO_PAL(vma_prot, vma_flags);
    if (PalVirtualMemoryProtect((void*)page, STUB_PAGE_SIZE,
                                PAL_PROT_READ | PAL_PROT_WRITE | PAL_PROT_EXEC | PAL_PROT_WRITECOPY) < 0) {
        log_debug("syscall-patch: mprotect RWX failed");
        goto out;
    }
    __atomic_store_n(site, 0x14000000u | (((uint32_t)(delta >> 2)) & 0x03ffffffu),
                     __ATOMIC_RELEASE);
    flush_icache((void*)page, STUB_PAGE_SIZE);
    if (PalVirtualMemoryProtect((void*)page, STUB_PAGE_SIZE, orig) < 0)
        BUG(); /* cannot leave application text writable */
    patched = true;
    g_patched_sites++;
    log_debug("syscall site %#lx rewritten -> stub %p (%zu sites patched)", pc, stub,
              g_patched_sites);

out:
    spinlock_unlock(&g_patch_lock);
    return patched;
}

/* A thread can trap on an `svc #0` and, before its SIGSYS handler re-reads the instruction, have
 * another thread rewrite that very site. The late handler must still treat the trap as a syscall
 * (the PAL already reverted pc to the site). Recognize it: the word at pc is now a `b` into one of
 * our stub pages. Lock-free: g_stub_pages entries are immutable once published. */
bool libos_syscall_site_was_patched(uintptr_t pc) {
    uint32_t insn = *(volatile uint32_t*)pc;
    if ((insn & 0xfc000000u) != 0x14000000u) /* not an unconditional `b` */
        return false;
    int64_t imm26 = insn & 0x03ffffffu;
    if (imm26 & (1 << 25))
        imm26 -= (1ll << 26);
    uintptr_t target = pc + (uintptr_t)(imm26 * 4);

    size_t cnt = __atomic_load_n(&g_stub_pages_cnt, __ATOMIC_ACQUIRE);
    for (size_t i = 0; i < cnt; i++) {
        if (target >= g_stub_pages[i].base && target < g_stub_pages[i].base + STUB_PAGE_SIZE)
            return true;
    }
    return false;
}
