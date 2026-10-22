/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/*
 * PAL_TCB lookup for the vbs PAL on AArch64: the same single-thread fallback cell and TLS-keyed
 * registry as the direct PAL (copied from pal/src/host/linux/arch/aarch64/pal_arch_exception.c,
 * without its Linux signal code). The LibOS's syscall entry (libos/src/arch/aarch64/syscallas.S)
 * and pal_get_tcb() (pal/include/arch/aarch64/pal_arch.h) read these symbols.
 */

#include <assert.h>
#include <stdatomic.h>

#include "pal_internal.h"

/* Milestone A stopgap single-threaded PAL_TCB storage -- see the comment on this declaration in
 * pal/include/arch/aarch64/pal_arch.h for why aarch64 can't just do what %gs does on x86_64.
 * "protected" visibility here must match the extern declaration's attribute in pal_arch.h (see
 * the comment there) -- it's this definition's own attribute that actually governs the symbol's
 * flag in libpal.so's dynamic symbol table. */
__attribute__((visibility("protected"))) PAL_TCB* g_pal_tcb_singleton = NULL;

#define LIBOS_TCB_REGISTRY_MAX 128

struct libos_tcb_registry_entry {
    _Atomic(uintptr_t) key; /* guest TLS pointer | 1; 0 = empty */
    _Atomic(PAL_TCB*) tcb;
};

/* Non-static: referenced (via :got:, cross-object) by libsysdb.so's syscallas.S scan and C code,
 * and directly (same object) by PAL code here. */
__attribute__((visibility("protected")))
/* Alignment must equal the total table size (LIBOS_TCB_REGISTRY_MAX * sizeof(entry)) so the
 * assembly scan in libos/src/arch/aarch64/syscallas.S can detect the end of a full-table scan
 * with `tst x17, #(sizeof(table)-1)` -- the cursor realigns to the table size exactly once past
 * the last entry. Do not shrink this alignment without updating that scan. */
struct libos_tcb_registry_entry g_pal_tcb_registry[LIBOS_TCB_REGISTRY_MAX]
    __attribute__((aligned(LIBOS_TCB_REGISTRY_MAX * sizeof(struct libos_tcb_registry_entry))));

/* syscallas.S's scan hardcodes the 16-byte stride and the tcb at +8, and terminates its
 * full-table walk with `tst x17, #(LIBOS_TCB_REGISTRY_MAX*16 - 1)` == 0 -- valid only at the
 * table's exact size and alignment (see the aligned attribute above). */
static_assert(sizeof(struct libos_tcb_registry_entry) == 16, "registry entry must be 16 bytes");
static_assert(offsetof(struct libos_tcb_registry_entry, tcb) == 8, "registry tcb must be at +8");
static_assert(sizeof(g_pal_tcb_registry) == 2048,
              "registry table size must stay 2048 (syscallas.S scan terminator constant)");

__attribute__((visibility("protected"))) void libos_tcb_registry_add(PAL_TCB* tcb,
                                                                    uintptr_t guest_tp) {
    if (!tcb || !guest_tp) {
        return; /* set_default_tls(0) etc.: not a thread identity, nothing to anchor */
    }
    uintptr_t key = guest_tp | 1;

    for (size_t i = 0; i < LIBOS_TCB_REGISTRY_MAX; i++) {
        if (atomic_load_explicit(&g_pal_tcb_registry[i].key, memory_order_acquire) == key) {
            /* TLS-address reuse (e.g. recycled mmap region): rebind the slot to the new owner. */
            atomic_store_explicit(&g_pal_tcb_registry[i].tcb, tcb, memory_order_release);
            return;
        }
    }
    for (size_t i = 0; i < LIBOS_TCB_REGISTRY_MAX; i++) {
        uintptr_t expected = 0;
        if (atomic_compare_exchange_strong_explicit(&g_pal_tcb_registry[i].key, &expected, key,
                                                    memory_order_acq_rel, memory_order_acquire)) {
            atomic_store_explicit(&g_pal_tcb_registry[i].tcb, tcb, memory_order_release);
            return;
        }
    }
    static _Atomic bool warned;
    if (!atomic_exchange(&warned, true)) {
        log_warning("PAL_TCB registry full (%u threads): further threads fall back to the main"
                    " thread's PAL_TCB and will corrupt each other",
                    LIBOS_TCB_REGISTRY_MAX);
    }
}

__attribute__((visibility("protected"))) void libos_tcb_registry_del(uintptr_t guest_tp,
                                                                     PAL_TCB* expected_tcb) {
    if (!guest_tp) {
        return;
    }
    uintptr_t key = guest_tp | 1;
    for (size_t i = 0; i < LIBOS_TCB_REGISTRY_MAX; i++) {
        if (atomic_load_explicit(&g_pal_tcb_registry[i].key, memory_order_acquire) == key) {
            /* Ownership check: delete only the caller's *own* binding. A key is just a TLS
             * address, and exec sweeps / normal stack mmap reuse hand a dying thread's old TLS
             * address to a new thread whose birth set_tls() hook re-registers (rebinds) this
             * very key -- a plain key match here would then delete the NEW owner's registration,
             * and that (live) thread would fall through pal_get_tcb() to the singleton fallback
             * and run its whole life as the main thread. If the slot already holds a different
             * TCB, the old binding was legitimately taken over: leave it alone. */
            if (atomic_load_explicit(&g_pal_tcb_registry[i].tcb, memory_order_acquire)
                    != expected_tcb) {
                return;
            }
            /* Clear the tcb first (a scan seeing {key, NULL} treats the slot as a miss --
             * transient or not), then release the slot itself for reuse by a future thread. */
            atomic_store_explicit(&g_pal_tcb_registry[i].tcb, NULL, memory_order_release);
            atomic_store_explicit(&g_pal_tcb_registry[i].key, 0, memory_order_release);
            return;
        }
    }
}

__attribute__((visibility("protected"))) bool libos_tcb_registry_scan(PAL_TCB** out_tcb) {
    uintptr_t tp;
    __asm__("mrs %0, tpidr_el0" : "=r"(tp));
    if (tp & 1) {
        return false; /* Saguaro-internal thread: handled by pal_get_tcb()'s tagged fast path */
    }
    uintptr_t key = tp | 1;
    for (size_t i = 0; i < LIBOS_TCB_REGISTRY_MAX; i++) {
        if (atomic_load_explicit(&g_pal_tcb_registry[i].key, memory_order_acquire) == key) {
            PAL_TCB* tcb = atomic_load_explicit(&g_pal_tcb_registry[i].tcb,
                                                memory_order_acquire);
            if (!tcb) {
                /* Slot claimed but tcb store not yet visible (or just deleted) -- transient
                 * during registration/deregistration; treat as a miss rather than return NULL. */
                return false;
            }
            *out_tcb = tcb;
            return true;
        }
    }
    return false;
}
