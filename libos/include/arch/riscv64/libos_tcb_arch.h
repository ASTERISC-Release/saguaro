/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

#pragma once

#include <stdint.h>

#include "pal.h"

/*
 * riscv64 floating-point state, mirroring `PAL_FPSTATE`/the kernel's `struct
 * __riscv_d_ext_state` byte-for-byte (see the comment on PAL_FPSTATE in pal_arch.h for why this
 * has to be exact, not just "equivalent"): the lp64d ABI always saves the 64-bit register width,
 * so there is no F-vs-D variant to pick; `fcsr` is the single control word (rounding mode +
 * sticky exception flags), where aarch64 splits the same information into FPSR/FPCR. Unlike
 * x86_64's XSAVE area, there is no variable-size "extended state" concept for scalar F/D --
 * vector state is the only optional record (out of scope for now, see the plan's decisions), so
 * `struct libos_xstate` is always exactly this fixed size and no magic-number validation scheme
 * is needed.
 */
#define LIBOS_XSTATE_ALIGN 16

struct libos_fpstate {
    uint64_t f[32];
    uint32_t fcsr;
};

struct libos_xstate {
    struct libos_fpstate fpstate;
} __attribute__((aligned(LIBOS_XSTATE_ALIGN)));

/*
 * x86_64 reads/writes TCB fields via `%gs:offset`, a single-instruction addressing mode with no
 * riscv64 equivalent (no segment registers). `tp` holds the same thing %gs_base holds on x86_64
 * -- a pointer to the thread's TLS block -- but on riscv64 (exactly as on aarch64) it belongs to
 * the *application's* libc, so the PAL TCB is not in it; we materialize the PAL_TCB via
 * pal_get_tcb() (tagged-`tp` fast path, registry scan, singleton fallback) and then do a
 * completely ordinary C field access. This compiles to the same few real instructions that the
 * x86_64 macro's single %gs-relative instruction represents; it's just not expressible as one
 * instruction on this architecture.
 */
#define LIBOS_TCB_GET(member) \
    (((libos_tcb_t*)((uint8_t*)pal_get_tcb() + offsetof(PAL_TCB, libos_tcb)))->member)

#define LIBOS_TCB_SET(member, value)                                                      \
    do {                                                                                  \
        ((libos_tcb_t*)((uint8_t*)pal_get_tcb() + offsetof(PAL_TCB, libos_tcb)))->member  \
            = (value);                                                                    \
    } while (0)

/* set_tls() below registers the thread in the per-thread PAL_TCB registry (declared in
 * pal_arch.h via the pal.h include above; defined in libpal's pal_arch_exception.c -- see the
 * comment there). */

/* `tp` is writable directly from user mode (`mv tp, reg`) -- unlike x86_64's %fs, which needs the
 * arch_prctl(ARCH_SET_FS, ...) syscall, no syscall is involved here at all. */
static inline void set_tls(uintptr_t tls) {
    /* Register *before* the mv: while the old (tagged) `tp` is still in place, pal_get_tcb()
     * resolves this thread's own PAL_TCB correctly, which is the value to anchor to the new TLS
     * key. For the application's clone()/fork() children this is their first action; after it
     * the registry (or, pre-registration, pal_get_tcb()'s singleton fallback) is the only way to
     * reach the TCB. */
    libos_tcb_registry_add(pal_get_tcb(), tls);
    __asm__ volatile("mv tp, %0" :: "r"(tls) : "memory");
}

static inline void set_default_tls(void) {
    set_tls(0);
}

static inline uintptr_t get_tls(void) {
    uintptr_t addr;
    __asm__ volatile("mv %0, tp" : "=r"(addr));
    return addr;
}

/*
 * Per-thread PAL_TCB lifecycle hooks (shared code calls these; x86_64 has no equivalents because
 * its PAL_TCB lives in a segment register the guest never touches).
 *
 * On riscv64 the guest owns `tp`, so a thread's PAL_TCB is reached through a TLS-keyed registry
 * plus a one-shot snapshot (see pal_arch.h and findings #9, #12 and #13 in
 * dev docs: aarch64/direct/FINDINGS.md -- the design is reused unchanged).
 */

/*
 * Called right before set_default_tls() abandons the startup `tp` in preparation for the
 * application's own TLS setup (boot, and execve()). The thread's PAL_TCB must stay reachable via
 * g_pal_tcb_singleton for the code between here and the application entry point (e.g.
 * execute_elf_object()'s own vdso_map_init()/bkeep_mmap() bookkeeping), so it is snapshotted
 * now, while the tagged `tp` fast path still resolves it. See
 * pal_tcb_singleton_snapshot_before_user_entry()'s comment in pal_arch.h.
 */
static inline void libos_arch_before_user_entry(void) {
    pal_tcb_singleton_snapshot_before_user_entry();
}

/*
 * execve(): abandon the pre-execve TLS pointer by deleting this thread's registry entry now. Its
 * old main-stack region is about to be munmapped and routinely re-mmapped as the *new* image's
 * main stack -- or a new pthread's stack -- by a *different* thread, which must not resolve
 * through this stale binding (see g_pal_tcb_registry's comment in pal_arch_exception.c).
 * Race-free: sibling threads are already killed at this point, and this thread's own
 * registrations for the new TLS pointer happen lazily (scan miss -> singleton, which is exactly
 * this thread thanks to libos_arch_before_user_entry()). The deletion is ownership-checked with
 * the same pal_get_tcb() value the snapshot stored (see _PalThreadExit()'s comment).
 */
static inline void libos_arch_exec_abandon_old_tls(void) {
    libos_tcb_registry_del(get_tls(), pal_get_tcb());
}

/*
 * Thread exit: drop this thread's TLS-keyed registry binding *before* it removes itself from the
 * thread list -- that removal is what releases the exec winner's kill_other_threads() loop, which
 * then unmaps (and, via mmap reuse, hands to new threads) the address region this binding is
 * keyed by -- and take `tp` over for Saguaro's own use (tagged-PAL_TCB fast path) so every
 * remaining lookup on this host thread (check_last_thread(), the async cleanup,
 * _PalThreadExit()'s host-stack recycling, the wait-forever park) keeps resolving *this* thread's
 * own PAL_TCB without a registry entry. This thread never runs guest code again from here, and a
 * live thread's `tp` is unique, so nothing can legitimately collide with the abandoned TLS
 * address anymore.
 *
 * Ordering matters: the own-PAL_TCB must be resolved *before* deregistering, since the
 * deregistration makes this thread's own next pal_get_tcb() miss the registry and fall back to
 * the singleton -- the boot/exec-winner thread's PAL_TCB, i.e. a *different* thread. The
 * deletion itself is ownership-checked for the same reason (_PalThreadExit()'s comment).
 */
static inline void libos_arch_thread_exit_release_tcb(void) {
    uintptr_t exiting_tcb = (uintptr_t)pal_get_tcb() | 1;
    libos_tcb_registry_del(get_tls(), (PAL_TCB*)(exiting_tcb & ~(uintptr_t)1));
    __asm__ volatile("mv tp, %0" :: "r"(exiting_tcb) : "memory");
}
