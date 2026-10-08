/* SPDX-License-Identifier: LGPL-3.0-or-later */

#pragma once

#include <stdint.h>

#include "pal.h"

/*
 * aarch64 FP/SIMD state, mirroring `PAL_FPSTATE`/`struct fpsimd_context` byte-for-byte (see the
 * comment on PAL_FPSTATE in pal_arch.h for why this has to be exact, not just "equivalent"):
 * magic+size identify the record, then FPSR/FPCR, then the 32 128-bit V registers. Unlike
 * x86_64's XSAVE area, there is no variable-size "extended state" concept here (for now, scope
 * of the aarch64 port: NEON only, no SVE/SVE2), so `struct libos_xstate` is always exactly this
 * fixed size -- no magic-number software-validation scheme is needed.
 */
#define LIBOS_XSTATE_ALIGN 16

struct libos_fpstate {
    uint32_t magic;
    uint32_t size;
    uint32_t fpsr;
    uint32_t fpcr;
    __uint128_t vregs[32];
};

struct libos_xstate {
    struct libos_fpstate fpstate;
} __attribute__((aligned(LIBOS_XSTATE_ALIGN)));

/*
 * x86_64 reads/writes TCB fields via `%gs:offset`, a single-instruction addressing mode with no
 * aarch64 equivalent (no segment registers). TPIDR_EL0 holds the same thing %gs_base holds on
 * x86_64 -- a pointer to PAL_TCB -- but as a system register, not an implicit memory-addressing
 * base, so we materialize it into a GPR via pal_get_tcb() (itself just one `mrs`) and then do a
 * completely ordinary C field access. This compiles to the same two real instructions (read the
 * base, then load/store at a constant offset) that the x86_64 macro's single %gs-relative
 * instruction represents; it's just not expressible as one instruction on this architecture.
 */
#define LIBOS_TCB_GET(member) \
    (((libos_tcb_t*)((uint8_t*)pal_get_tcb() + offsetof(PAL_TCB, libos_tcb)))->member)

#define LIBOS_TCB_SET(member, value)                                                     \
    do {                                                                                  \
        ((libos_tcb_t*)((uint8_t*)pal_get_tcb() + offsetof(PAL_TCB, libos_tcb)))->member  \
            = (value);                                                                    \
    } while (0)

/* set_tls() below registers the thread in the per-thread PAL_TCB registry (declared in
 * pal_arch.h via the pal.h include above; defined in libos_thread.c -- see the comment there). */

/* TPIDR_EL0 is writable directly from EL0 -- unlike x86_64's %fs, which needs the
 * arch_prctl(ARCH_SET_FS, ...) syscall, no syscall is involved here at all. */
static inline void set_tls(uintptr_t tls) {
    /* Register *before* the msr: while the old (tagged) TPIDR_EL0 is still in place,
     * pal_get_tcb() resolves this thread's own PAL_TCB correctly, which is the value to anchor
     * to the new TLS key. For the application's clone()/fork() children this is their first
     * action; after it the registry (or, pre-registration, pal_get_tcb()'s singleton fallback)
     * is the only way to reach the TCB. */
    libos_tcb_registry_add(pal_get_tcb(), tls);
    __asm__ volatile("msr tpidr_el0, %0" :: "r"(tls) : "memory");
}

static inline void set_default_tls(void) {
    set_tls(0);
}

static inline uintptr_t get_tls(void) {
    uintptr_t addr;
    __asm__ volatile("mrs %0, tpidr_el0" : "=r"(addr));
    return addr;
}
