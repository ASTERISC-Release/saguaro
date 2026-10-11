/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */
/* Copyright (C) 2014 Stony Brook University
 *               2020 Intel Labs
 */

/*
 * The sigaction/sigprocmask logic here has no riscv64-specific content at all -- it only deals
 * with __sigset_t/sigaction struct layout (per-arch headers) and the syscall_rt_sigreturn
 * trampoline (per-arch asm), both supplied by the riscv64 sibling headers/syscall.S, so that
 * part of the C code is identical to the x86_64/aarch64 versions. One genuine riscv64
 * difference: glibc/binutils for riscv64 have *no* `sa_restorer` field in struct sigaction at all
 * (the riscv kernel, like arm64, does not use the x86 SA_RESTORER mechanism -- it points `ra` at
 * the vDSO's __kernel_rt_sigreturn itself on signal delivery), so the x86 lines that set
 * SA_RESTORER/sa_restorer are absent here; the LibOS side has to supply its own handler-return
 * trampoline just like aarch64 finding #5 (see libos_context.c).
 */

#include <stddef.h> /* needed by <linux/signal.h> for size_t */
#include <linux/signal.h>

#include <assert.h>  /* static_assert for the PAL_TCB registry layout */
#include <stdatomic.h>

#include "pal_internal.h"

#include "sigreturn.h"
#include "sigset.h"
#include "syscall.h"
#include "ucontext.h" /* pulls in pal.h -> pal_arch.h, which declares g_pal_tcb_singleton */

/* Single-threaded PAL_TCB storage -- see the comment on this declaration in
 * pal/include/arch/riscv64/pal_arch.h for why riscv64 can't just do what %gs does on x86_64 (the
 * thread pointer belongs to the application's libc, same situation as aarch64).
 * "protected" visibility here must match the extern declaration's attribute in pal_arch.h (see
 * the comment there) -- it's this definition's own attribute that actually governs the symbol's
 * flag in libpal.so's dynamic symbol table. */
__attribute__((visibility("protected"))) PAL_TCB* g_pal_tcb_singleton = NULL;

int arch_do_rt_sigprocmask(int sig, int how) {
    __sigset_t mask;
    __sigemptyset(&mask);
    __sigaddset(&mask, sig);

    return DO_SYSCALL(rt_sigprocmask, how, &mask, NULL, sizeof(__sigset_t));
}

int arch_do_rt_sigaction(int sig, void* handler,
                         const int* async_signals, size_t async_signals_cnt) {
    struct sigaction action = {0};
    action.sa_handler  = handler;
    action.sa_flags    = SA_SIGINFO | SA_ONSTACK;
    /* No SA_RESTORER/sa_restorer on riscv64: the field does not exist in this ABI's struct
     * sigaction (see the file comment). */

    /* disallow nested asynchronous signals during exception handling */
    __sigemptyset((__sigset_t*)&action.sa_mask);
    for (size_t i = 0; i < async_signals_cnt; i++)
        __sigaddset((__sigset_t*)&action.sa_mask, async_signals[i]);

    return DO_SYSCALL(rt_sigaction, sig, &action, NULL, sizeof(__sigset_t));
}

/*
 * Per-thread PAL_TCB registry (riscv64 multi-threading; reused design from the aarch64 port, see
 * findings #9, #12 and #13 in Documentation/devel/aarch64/direct-pal-port-log.md).
 *
 * riscv64 has exactly one user-writable TLS register (`tp`) and the application's libc needs it,
 * so a thread's PAL_TCB cannot live in a register the way x86_64 keeps it in %gs. What every
 * thread *does* have is a unique application TLS pointer (glibc's TCB self pointer, installed via
 * set_tls() at clone/fork-thread creation; for the main thread, the initial TLS block its ld.so
 * or static startup plants in `tp`). This table maps that per-thread-unique key (stored as
 * `key = guest_tp | 1`; real TLS pointers are aligned so bit 0 is otherwise zero, and 0 stays the
 * empty-slot marker) to the thread's own PAL_TCB, which is how syscallas.S's
 * `libos_syscall_entry` prologue and pal_get_tcb() (both objects, see pal_arch.h) resolve the
 * *current* thread's TCB without owning the TLS register.
 *
 * The table lives in libpal.so (like g_pal_tcb_singleton above) -- not in libsysdb.so -- because
 * PAL code running on an application host thread (`tp` untagged, e.g. `_PalThreadExit()`'s
 * thread-stack recycling) must resolve the *exiting thread's own* PAL_TCB. libsysdb reaches this
 * table through the GOT (same proven pattern as g_pal_tcb_singleton), so the syscallas.S scan
 * loads its base with `la` (which assembles to the GOT-load pair for dynamic symbols).
 *
 * The scan in syscallas.S is a plain linear walk (up to LIBOS_TCB_REGISTRY_MAX entries), so the
 * entry layout {key, tcb} and this size must stay in sync with it.
 *
 * Registration: every non-main thread registers itself in set_tls() (its *first* act, before any
 * syscall can trap). A scan miss is a thread that installed its `tp` through an unhooked path --
 * the boot main thread, or the exec winner after the new image's ld.so re-set `tp` -- for both of
 * which the g_pal_tcb_singleton fallback *is* correct (exec snapshots it via
 * pal_tcb_singleton_snapshot_before_user_entry()), and pal_get_tcb() lazily self-registers
 * there. Deletion: an entry must be deleted when its owner abandons that TLS pointer -- i.e. in
 * `_PalThreadExit()` (thread death, after the last own resolution, before the asm stub) and in
 * the exec winner right after its singleton snapshot (abandoning the pre-exec TLS pointer).
 * Without this, a *stale* binding whose owner execed away or died can be re-hit by an unrelated
 * thread whose `tp` lands on the abandoned address after mmap reuse, and that thread then
 * resolves to a dead/foreign PAL_TCB. A *live* thread's binding can never be mis-hit this way
 * (`tp` is unique per live thread), which is why the wait-forever main-thread entry is
 * deliberately kept (its own emulated park-loop syscalls resolve through it). Writers run only in
 * per-thread context (the registering/exiting thread itself) or in the single-threaded window of
 * an exec, so a CAS per slot is all the locking needed.
 */
#define LIBOS_TCB_REGISTRY_MAX 128

struct libos_tcb_registry_entry {
    _Atomic(uintptr_t) key; /* guest TLS pointer | 1; 0 = empty */
    _Atomic(PAL_TCB*) tcb;
};

/* Non-static: referenced (via GOT, cross-object) by libsysdb.so's syscallas.S scan and C code,
 * and directly (same object) by PAL code here. */
__attribute__((visibility("protected")))
/* Alignment must equal the total table size (LIBOS_TCB_REGISTRY_MAX * sizeof(entry)) so the
 * assembly scan in libos/src/arch/riscv64/syscallas.S can detect the end of a full-table scan
 * with `andi t?, %(sizeof(table)-1)` -- the cursor realigns to the table size exactly once past
 * the last entry. Do not shrink this alignment without updating that scan. */
struct libos_tcb_registry_entry g_pal_tcb_registry[LIBOS_TCB_REGISTRY_MAX]
    __attribute__((aligned(LIBOS_TCB_REGISTRY_MAX * sizeof(struct libos_tcb_registry_entry))));

/* syscallas.S's scan hardcodes the 16-byte stride and the tcb at +8, and terminates its
 * full-table walk with `t0 & (LIBOS_TCB_REGISTRY_MAX*16 - 1) == 0` -- valid only at the table's
 * exact size and alignment (see the aligned attribute above). */
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
    __asm__("mv %0, tp" : "=r"(tp));
    if (tp & 1) {
        return false; /* Gramine-internal thread: handled by pal_get_tcb()'s tagged fast path */
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

/*
 * SIGTRAP handler for the exact-context-restore protocol (see PAL_BRK_RESTORE_CONTEXT in
 * pal_arch.h). The LibOS executes `ebreak` with a0 = a PAL_CONTEXT* and a7 = the 64-bit
 * discriminator; copying that context into the host ucontext makes the kernel's own sigreturn
 * restore all registers exactly. RISC-V's `ebreak` has no immediate field (unlike aarch64's
 * `brk #imm`, which is self-identifying), hence the a7 payload: an application `ebreak` cannot
 * plausibly have a7 == 0x6772677267726772, and anything else keeps its previous behavior:
 * restore the default action and return, so the instruction re-executes and the process dies as
 * it did before this handler existed (core dump, like a plain ebreak without Gramine).
 */
void pal_arch_handle_trap(ucontext_t* uc) {
    uint32_t insn = *(const uint32_t*)ucontext_get_ip(uc);
    if (insn == 0x00000007u /* ebreak */
            && uc->uc_mcontext.sc_regs.a7 == PAL_BRK_RESTORE_CONTEXT) {
        PAL_CONTEXT* ctx = (PAL_CONTEXT*)uc->uc_mcontext.sc_regs.a0;
        ctx->pstate &= ~PAL_PSTATE_EXACT_CONTEXT;
        pal_context_to_ucontext(uc, ctx);
        return;
    }
    arch_do_rt_sigaction(SIGTRAP, SIG_DFL, NULL, 0);
}
