/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/*
 * Memory of the vbs PAL proper.
 *
 * Inside a VBS enclave no memory can ever become executable at run time (phase 0, step c). The PAL
 * therefore deals with two kinds of memory:
 *   - the pool: read-write memory the Windows enclave runtime (W) committed at boot; every
 *     anonymous allocation of the PAL and the LibOS comes from here and is never executable;
 *   - the packed images (`.gxpack`, executable image pages): code and data of the objects packed at
 *     build time. Their writable segments may be made read-write (one way), their code is used in
 *     place and is never made writable (that would make it unusable for good, and executing it
 *     afterwards would hang the thread: blocker 4).
 * The PAL's memory window is the whole enclave range; everything except the pool and the extents of
 * the objects the LibOS maps (the application, the vDSO) is an initial range the PAL keeps for
 * itself (the image, W's stacks and heap).
 */

#include "api.h"
#include "elf/elf.h"
#include "pal.h"
#include "pal_error.h"
#include "pal_internal.h"
#include "pal_vbs.h"

/* ---- packed objects ---- */

const uint8_t* vbs_file_data(const struct pal_vbs_packed_obj* obj) {
    return (const uint8_t*)(uintptr_t)(g_vbs->files_base + obj->file_offset);
}

uint8_t* vbs_image_base(const struct pal_vbs_packed_obj* obj) {
    return (uint8_t*)(uintptr_t)(g_vbs->pack_base + obj->image_offset);
}

int vbs_uri_to_path(const char* uri, char* out, size_t size) {
    if (strstartswith(uri, URI_PREFIX_FILE))
        uri += URI_PREFIX_FILE_LEN;
    else if (strstartswith(uri, URI_PREFIX_DIR))
        uri += URI_PREFIX_DIR_LEN;
    /* relative paths are relative to "/" (there is no host working directory) */
    size_t len = 0;
    if (uri[0] != '/') {
        if (size < 2)
            return -PAL_ERROR_TOOLONG;
        out[len++] = '/';
    }
    size_t n = strlen(uri);
    if (len + n + 1 > size)
        return -PAL_ERROR_TOOLONG;
    memcpy(out + len, uri, n + 1);
    /* canonicalize "/./", "//", trailing "/" (".." is not used by the LibOS's URIs) */
    char* r = out;
    char* w = out;
    while (*r) {
        if (r[0] == '/' && (r[1] == '/' || (r[1] == '.' && (r[2] == '/' || r[2] == '\0')))) {
            r += (r[1] == '/') ? 1 : 2;
            continue;
        }
        *w++ = *r++;
    }
    *w = '\0';
    if (w > out + 1 && w[-1] == '/')
        w[-1] = '\0';
    if (out[0] == '\0') {
        out[0] = '/';
        out[1] = '\0';
    }
    return 0;
}

const struct pal_vbs_packed_obj* vbs_find_path(const char* path) {
    for (uint64_t i = 0; i < g_vbs->nobjs; i++)
        if (g_vbs->objs[i].path[0] && !strcmp(g_vbs->objs[i].path, path))
            return &g_vbs->objs[i];
    return NULL;
}

/* executable PT_LOADs of a packed object, as [lo, hi) page ranges in memory; returns the count */
static size_t packed_code_ranges(const struct pal_vbs_packed_obj* obj, uintptr_t* lo,
                                 uintptr_t* hi, size_t max) {
    if (!obj->image_size)
        return 0;
    const uint8_t* base = vbs_image_base(obj);
    const elf_ehdr_t* eh = (const elf_ehdr_t*)base;
    const elf_phdr_t* ph = (const elf_phdr_t*)(base + eh->e_phoff);
    size_t n = 0;
    for (size_t i = 0; i < eh->e_phnum && n < max; i++) {
        if (ph[i].p_type != PT_LOAD || !(ph[i].p_flags & PF_X))
            continue;
        lo[n] = (uintptr_t)base + ALLOC_ALIGN_DOWN(ph[i].p_vaddr);
        hi[n] = (uintptr_t)base + ALLOC_ALIGN_UP(ph[i].p_vaddr + ph[i].p_memsz);
        n++;
    }
    /* the svc stubs (between the last PT_LOAD and the slot page) are code too */
    if (obj->slot_offset && n < max) {
        lo[n] = (uintptr_t)base + ALLOC_ALIGN_UP(obj->map_size);
        hi[n] = (uintptr_t)base + obj->slot_offset;
        n++;
    }
    return n;
}

bool vbs_overlaps_packed_code(uintptr_t addr, size_t size) {
    if (!vbs_in_pack(addr, size))
        return false;
    for (uint64_t i = 0; i < g_vbs->nobjs; i++) {
        uintptr_t lo[8], hi[8];
        size_t n = packed_code_ranges(&g_vbs->objs[i], lo, hi, ARRAY_SIZE(lo));
        for (size_t j = 0; j < n; j++)
            if (addr < hi[j] && lo[j] < addr + size)
                return true;
    }
    return false;
}

bool vbs_inside_packed_code(uintptr_t addr, size_t size) {
    for (uint64_t i = 0; i < g_vbs->nobjs; i++) {
        uintptr_t lo[8], hi[8];
        size_t n = packed_code_ranges(&g_vbs->objs[i], lo, hi, ARRAY_SIZE(lo));
        for (size_t j = 0; j < n; j++)
            if (lo[j] <= addr && addr + size <= hi[j])
                return true;
    }
    return false;
}

bool vbs_in_pool(uintptr_t addr, size_t size) {
    return addr >= g_vbs->pool_base && size <= g_vbs->pool_size &&
           addr - g_vbs->pool_base <= g_vbs->pool_size - size;
}

bool vbs_in_pack(uintptr_t addr, size_t size) {
    return addr >= g_vbs->pack_base && size <= g_vbs->pack_size &&
           addr - g_vbs->pack_base <= g_vbs->pack_size - size;
}

const struct pal_vbs_packed_obj* g_vbs_libos_obj; /* set when pal_rtld.c loads the LibOS */

int vbs_packed_lookup(const char* uri, uintptr_t* out_base, size_t* out_size,
                      uintptr_t* out_syscall_slot) {
    const struct pal_vbs_packed_obj* obj;
    if (!uri) {
        /* the LibOS itself (its own svc stubs, e.g. its rt_sigreturn trampoline) */
        obj = g_vbs_libos_obj;
        if (!obj)
            return -PAL_ERROR_STREAMNOTEXIST;
    } else {
        char path[URI_MAX];
        int ret = vbs_uri_to_path(uri, path, sizeof(path));
        if (ret < 0)
            return ret;
        obj = vbs_find_path(path);
        if (!obj || obj->kind != PAL_VBS_KIND_USER)
            return -PAL_ERROR_STREAMNOTEXIST;
    }
    uintptr_t base = (uintptr_t)vbs_image_base(obj);
    uintptr_t slot = 0;
    if (obj->slot_offset) {
        /* the slot page holds data only; it becomes read-write for the LibOS to fill */
        slot = base + obj->slot_offset;
        if (vbs_protect(slot, g_page_size, PAL_PROT_READ | PAL_PROT_WRITE) < 0)
            return -PAL_ERROR_DENIED;
    }
    *out_base = base;
    *out_size = obj->map_size;
    *out_syscall_slot = slot;
    return 0;
}

/* ---- PAL memory API ---- */

/*
 * Pool memory that was never handed out since boot is still zero (W committed it with
 * VirtualAlloc), so a first allocation need not clear it. Clearing it anyway costs a first touch of
 * every page, which is slow in a VBS enclave (CI-Examples/helloworld maps 512 MiB it never uses: 3-8
 * s). One bit per 64 KiB granule of the pool says whether any part of it was ever allocated.
 */
#define POOL_GRANULE_SHIFT 16
#define POOL_MAX_SIZE      (64ull << 30) /* the largest vbs.enclave_size */
static uint8_t g_pool_used[POOL_MAX_SIZE >> POOL_GRANULE_SHIFT >> 3];

static bool pool_granule_used(size_t g) {
    return g_pool_used[g >> 3] & (1u << (g & 7));
}

/* clears [a, a + size) of the pool except whole granules that were never allocated, and marks every
 * granule the range touches as allocated */
static void pool_clear(uintptr_t a, size_t size) {
    if (g_vbs->pool_size > POOL_MAX_SIZE) {
        memset((void*)a, 0, size);
        return;
    }
    uintptr_t end = a + size;
    uintptr_t base = g_vbs->pool_base;
    uintptr_t cur = a;
    while (cur < end) {
        size_t g = (cur - base) >> POOL_GRANULE_SHIFT;
        uintptr_t g_lo = base + ((uintptr_t)g << POOL_GRANULE_SHIFT);
        uintptr_t g_hi = g_lo + (1ul << POOL_GRANULE_SHIFT);
        uintptr_t lo = cur, hi = MIN(end, g_hi);
        if (pool_granule_used(g) || lo != g_lo || hi != g_hi)
            memset((void*)lo, 0, hi - lo);
        __atomic_fetch_or(&g_pool_used[g >> 3], (uint8_t)(1u << (g & 7)), __ATOMIC_RELAXED);
        cur = hi;
    }
}

int _PalVirtualMemoryAlloc(void* addr, size_t size, pal_prot_flags_t prot) {
    assert(WITHIN_MASK(prot, PAL_PROT_MASK));
    uintptr_t a = (uintptr_t)addr;
    if (prot & PAL_PROT_EXEC) {
        log_error("vbs: anonymous executable memory is impossible in a VBS enclave (%p, %#lx)",
                  addr, size);
        return -PAL_ERROR_DENIED;
    }
    if (!vbs_in_pool(a, size) && !(vbs_in_pack(a, size) && !vbs_overlaps_packed_code(a, size))) {
        log_error("vbs: allocation outside the pool and the packed data (%p, %#lx)", addr, size);
        return -PAL_ERROR_NOMEM;
    }
    /* fresh memory reads as zeros: make it writable, clear it, then set the protection asked */
    int64_t ret = vbs_protect(a, size, PAL_PROT_READ | PAL_PROT_WRITE);
    if (ret < 0) {
        log_error("vbs: protect(%p, %#lx, RW) failed: %ld", addr, size, ret);
        return -PAL_ERROR_DENIED;
    }
    if (vbs_in_pool(a, size))
        pool_clear(a, size);
    else
        memset(addr, 0, size);
    pal_prot_flags_t p = prot & (PAL_PROT_READ | PAL_PROT_WRITE);
    if (p != (PAL_PROT_READ | PAL_PROT_WRITE) && vbs_protect(a, size, p) < 0) {
        /* a weaker protection than asked is harmless here; note it once */
        if (FIRST_TIME())
            log_warning("vbs: protect(%p, %#lx, %#x) failed; memory stays read-write", addr,
                        size, p);
    }
    return 0;
}

int _PalVirtualMemoryFree(void* addr, size_t size) {
    uintptr_t a = (uintptr_t)addr;
    if (vbs_in_pool(a, size)) {
        /* nothing is returned to Windows: the pool stays committed; make it inaccessible so a use
         * after free faults (if PAGE_NOACCESS is refused, it stays as it was) */
        (void)vbs_protect(a, size, 0);
        return 0;
    }
    /* packed images cannot be freed; code must never lose its execute permission */
    return 0;
}

int _PalVirtualMemoryProtect(void* addr, size_t size, pal_prot_flags_t prot) {
    uintptr_t a = (uintptr_t)addr;
    if (vbs_overlaps_packed_code(a, size)) {
        /* Packed code stays execute-read: asking for exactly that is a no-op; anything that would
         * remove the execute permission or add write is refused or ignored -- changing it would
         * make the code unusable forever (the platform allows no way back). */
        if (prot & PAL_PROT_WRITE) {
            log_error("vbs: refusing to make packed code writable (%p, %#lx)", addr, size);
            return -PAL_ERROR_DENIED;
        }
        if (!vbs_inside_packed_code(a, size)) {
            log_error("vbs: protection change across packed code and data (%p, %#lx)", addr, size);
            return -PAL_ERROR_DENIED;
        }
        return 0;
    }
    if (prot & PAL_PROT_EXEC) {
        log_error("vbs: memory cannot become executable in a VBS enclave (%p, %#lx)", addr, size);
        return -PAL_ERROR_DENIED;
    }
    int64_t ret = vbs_protect(a, size, prot);
    if (ret < 0) {
        log_error("vbs: protect(%p, %#lx, %#x) failed: %ld", addr, size, prot, ret);
        return -PAL_ERROR_DENIED;
    }
    return 0;
}

unsigned long _PalMemoryQuota(void) {
    return g_vbs->pool_size;
}

void pal_read_next_reserved_range(uintptr_t last_range_start, uintptr_t* out_next_range_start,
                                  uintptr_t* out_next_range_end) {
    __UNUSED(last_range_start);
    *out_next_range_start = 0;
    *out_next_range_end = 0;
}

/* ---- boot ---- */

struct free_area {
    uintptr_t start, end;
    bool user; /* an extent the LibOS maps with MAP_FIXED (application, vDSO) */
};

int vbs_init_memory(void) {
    uintptr_t win_lo = g_vbs->enclave_base;
    uintptr_t win_hi = g_vbs->enclave_base + g_vbs->enclave_size;
    if (!IS_ALLOC_ALIGNED(win_lo) || !IS_ALLOC_ALIGNED(win_hi) || !vbs_in_pool(g_vbs->pool_base, 1))
        return -PAL_ERROR_INVAL;

    /* the free areas, sorted by address: the pool and the user objects' extents -- one area over
     * all user objects when no PAL-side object lies between them (the PAL's table of initial
     * ranges is small), else one per user object */
    static struct free_area areas[256];
    size_t n = 0;
    areas[n++] = (struct free_area){ g_vbs->pool_base, g_vbs->pool_base + g_vbs->pool_size, false };
    uintptr_t ulo = UINTPTR_MAX, uhi = 0;
    for (uint64_t i = 0; i < g_vbs->nobjs; i++) {
        const struct pal_vbs_packed_obj* o = &g_vbs->objs[i];
        if (o->kind != PAL_VBS_KIND_USER)
            continue;
        uintptr_t b = (uintptr_t)vbs_image_base(o);
        ulo = MIN(ulo, b);
        uhi = MAX(uhi, b + ALLOC_ALIGN_UP(o->image_size));
    }
    bool merged = ulo < uhi;
    for (uint64_t i = 0; merged && i < g_vbs->nobjs; i++) {
        const struct pal_vbs_packed_obj* o = &g_vbs->objs[i];
        uintptr_t b = (uintptr_t)vbs_image_base(o);
        if (o->kind == PAL_VBS_KIND_PAL && o->image_size && b < uhi && ulo < b + o->image_size)
            merged = false;
    }
    if (merged) {
        areas[n++] = (struct free_area){ ulo, uhi, true };
    } else {
        for (uint64_t i = 0; i < g_vbs->nobjs; i++) {
            const struct pal_vbs_packed_obj* o = &g_vbs->objs[i];
            if (o->kind != PAL_VBS_KIND_USER)
                continue;
            if (n == ARRAY_SIZE(areas))
                return -PAL_ERROR_NOMEM;
            uintptr_t b = (uintptr_t)vbs_image_base(o);
            areas[n++] = (struct free_area){ b, b + ALLOC_ALIGN_UP(o->map_size), true };
        }
    }
    for (size_t i = 1; i < n; i++)
        for (size_t j = i; j > 0 && areas[j].start < areas[j - 1].start; j--) {
            struct free_area t = areas[j];
            areas[j] = areas[j - 1];
            areas[j - 1] = t;
        }

    /* everything else in the window is the PAL's: the image, W's stacks and heap */
    uintptr_t cur = win_lo;
    for (size_t i = 0; i <= n; i++) {
        uintptr_t next = (i < n) ? areas[i].start : win_hi;
        if (next < cur || next > win_hi)
            return -PAL_ERROR_INVAL; /* overlapping areas or outside the window */
        if (next > cur) {
            int ret = pal_add_initial_range(cur, next - cur, PAL_PROT_READ | PAL_PROT_WRITE,
                                            /*is_user_reserved=*/false, "vbs: enclave runtime");
            if (ret < 0)
                return ret;
        }
        if (i < n) {
            if (areas[i].user) {
                int ret = pal_add_initial_range(areas[i].start, areas[i].end - areas[i].start,
                                                PAL_PROT_READ | PAL_PROT_EXEC,
                                                /*is_user_reserved=*/true, "vbs: packed objects");
                if (ret < 0)
                    return ret;
            }
            cur = areas[i].end;
        }
    }

    g_pal_public_state.memory_address_start = (void*)win_lo;
    g_pal_public_state.memory_address_end = (void*)win_hi;
    g_pal_public_state.shared_address_start = (void*)win_lo;
    g_pal_public_state.shared_address_end = (void*)win_hi;
    return 0;
}
