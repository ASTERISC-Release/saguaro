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
#include <stdnoreturn.h>

#include "pal_internal.h"
#include "pal_linux.h"
#include "pal_linux_defs.h"

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

    /* The mask changes: what pal_arch_note_syscall_trap() recorded is no longer the mask to
     * return to the application with. (No TCB yet during early initialization.) */
    PAL_LINUX_TCB* tcb = pal_get_linux_tcb();
    if (tcb)
        tcb->trap_sigmask_valid = false;

    return DO_SYSCALL(rt_sigprocmask, how, &mask, NULL, sizeof(__sigset_t));
}

/* Called for every seccomp trap. The mask in the trap's frame is the thread's mask outside the
 * handler, which is what the application must get back when the emulated syscall returns;
 * recording it here saves pal_arch_restore_context() a rt_sigprocmask() call per syscall. */
void pal_arch_note_syscall_trap(ucontext_t* uc) {
    static_assert(sizeof(uc->uc_sigmask) >= sizeof(uint64_t), "kernel sigset_t is 64 bits");
    PAL_LINUX_TCB* tcb = pal_get_linux_tcb();
    memcpy(&tcb->trap_sigmask, &uc->uc_sigmask, sizeof(tcb->trap_sigmask));
    tcb->trap_sigmask_valid = true;
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
    /* Both widths of `ebreak` must be recognized. RISC-V's `ebreak` is `0x00100073` (`0x00000073` is `ecall`), but under
     * the toolchain's default `-march` (rv64imafdc, i.e. with the C extension) both gas and the
     * linker RELAX a plain `ebreak` mnemonic to the 2-byte compressed encoding `c.ebreak`
     * (`0x9002`) -- measured in the guest with scripts/riscv64/checks/ebreak_trap.c, which also
     * measured that the saved pc sits *at* the trapping instruction for both widths. A 32-bit
     * fetch at a compressed site would therefore read `{next insn's low half}<<16 | 0x9002` and
     * miss the protocol. Reading 16 bits first is always in-bounds: pc is 2-byte aligned (all
     * RISC-V instructions are), so a 2-byte load never straddles a page edge. */
    uint16_t insn16 = *(const uint16_t*)ucontext_get_ip(uc);
    bool is_ebreak = insn16 == 0x9002 /* c.ebreak */
                     || (insn16 == 0x0073 && *(const uint32_t*)ucontext_get_ip(uc) == 0x00100073);
    if (is_ebreak && uc->uc_mcontext.sc_regs.a7 == PAL_BRK_RESTORE_CONTEXT) {
        PAL_CONTEXT* ctx = (PAL_CONTEXT*)uc->uc_mcontext.sc_regs.a0;
        ctx->pstate &= ~PAL_PSTATE_EXACT_CONTEXT;
        /* A syscall-entry context carries no F/D register file, only fcsr (the LibOS saves no
         * FP registers at syscall entry). The live registers in `uc` are then the ones to
         * resume with; pal_context_to_ucontext() would zero them. */
        bool has_fpregs = ctx->is_fpregs_used && ctx->fpregs;
        struct riscv_d_ext_state live_fp = uc->uc_mcontext.sc_fpregs.d;
        pal_context_to_ucontext(uc, ctx);
        if (!has_fpregs) {
            live_fp.fcsr = ctx->fcsr;
            uc->uc_mcontext.sc_fpregs.d = live_fp;
        }
        return;
    }
    arch_do_rt_sigaction(SIGTRAP, SIG_DFL, NULL, 0);
}

/*
 * Resumes `ctx` exactly: all 31 general-purpose registers, the pc and the F/D state at once. Used
 * by the LibOS for every return to the application (libos/src/arch/riscv64/syscallas.S explains
 * why no register can be given up for a user-mode jump).
 *
 * Only the kernel can install a full register set, and rt_sigreturn() is the interface for it:
 * it takes the state from a signal frame at `sp`, whoever built that frame. We build one on the
 * current stack. Compared to the `ebreak` protocol above, which reaches the same sigreturn through
 * a trap and a signal delivery, this costs two plain syscalls.
 *
 * Besides the registers, sigreturn installs the signal mask and the alternate stack from the
 * frame, so both are filled with their current values.
 */
struct pal_rt_sigframe {
    siginfo_t info;
    ucontext_t uc;
};

noreturn void pal_arch_sigreturn_frame(void* rt_sigframe);

__asm__(
".pushsection .text\n"
".type pal_arch_save_fpregs, @function\n"
"pal_arch_save_fpregs:\n" /* a0: uint64_t f[32] */
    "fsd  f0,  0(a0)\n"  "fsd  f1,  8(a0)\n"  "fsd  f2, 16(a0)\n"  "fsd  f3, 24(a0)\n"
    "fsd  f4, 32(a0)\n"  "fsd  f5, 40(a0)\n"  "fsd  f6, 48(a0)\n"  "fsd  f7, 56(a0)\n"
    "fsd  f8, 64(a0)\n"  "fsd  f9, 72(a0)\n"  "fsd f10, 80(a0)\n"  "fsd f11, 88(a0)\n"
    "fsd f12, 96(a0)\n"  "fsd f13,104(a0)\n"  "fsd f14,112(a0)\n"  "fsd f15,120(a0)\n"
    "fsd f16,128(a0)\n"  "fsd f17,136(a0)\n"  "fsd f18,144(a0)\n"  "fsd f19,152(a0)\n"
    "fsd f20,160(a0)\n"  "fsd f21,168(a0)\n"  "fsd f22,176(a0)\n"  "fsd f23,184(a0)\n"
    "fsd f24,192(a0)\n"  "fsd f25,200(a0)\n"  "fsd f26,208(a0)\n"  "fsd f27,216(a0)\n"
    "fsd f28,224(a0)\n"  "fsd f29,232(a0)\n"  "fsd f30,240(a0)\n"  "fsd f31,248(a0)\n"
    "ret\n"
".popsection\n"
);
void pal_arch_save_fpregs(uint64_t* f);

__attribute__((visibility("protected"))) noreturn void pal_arch_restore_context(PAL_CONTEXT* ctx) {
    struct pal_rt_sigframe frame __attribute__((aligned(16)));

    /* A syscall-entry context carries no F/D register file, only fcsr (the LibOS saves no FP
     * registers at syscall entry, and neither it nor the PAL computes in floating point). The
     * live registers are then the application's, and go into the frame first thing. */
    bool has_fpregs = ctx->is_fpregs_used && ctx->fpregs;
    struct riscv_d_ext_state* fp = &frame.uc.uc_mcontext.sc_fpregs.d;
    if (!has_fpregs)
        pal_arch_save_fpregs(fp->f);

    frame.uc.uc_flags = 0;
    frame.uc.uc_link = NULL;

    PAL_LINUX_TCB* tcb = pal_get_linux_tcb();
    if (tcb->alt_stack) {
        frame.uc.uc_stack.ss_sp    = tcb->alt_stack;
        frame.uc.uc_stack.ss_flags = 0;
        frame.uc.uc_stack.ss_size  = ALT_STACK_SIZE - sizeof(*tcb);
    } else {
        frame.uc.uc_stack.ss_sp    = NULL;
        frame.uc.uc_stack.ss_flags = SS_DISABLE;
        frame.uc.uc_stack.ss_size  = 0;
    }

    /* The frame needs the mask to resume with. Normally that is the one recorded at this
     * syscall's trap; a thread that has not trapped yet, or whose mask the PAL changed since,
     * asks the kernel. */
    memset(&frame.uc.uc_sigmask, 0, sizeof(frame.uc.uc_sigmask));
    if (tcb->trap_sigmask_valid) {
        memcpy(&frame.uc.uc_sigmask, &tcb->trap_sigmask, sizeof(tcb->trap_sigmask));
    } else {
        DO_SYSCALL(rt_sigprocmask, SIG_SETMASK, NULL, &frame.uc.uc_sigmask, sizeof(__sigset_t));
    }

    ctx->pstate &= ~PAL_PSTATE_EXACT_CONTEXT;
    if (has_fpregs) {
        pal_context_to_ucontext(&frame.uc, ctx);
    } else {
        riscv_user_regs_from_array(&frame.uc.uc_mcontext.sc_regs, ctx->regs);
        frame.uc.uc_mcontext.sc_regs.pc = ctx->pc;
        fp->fcsr = ctx->fcsr;
    }

    /* No extension records (the kernel rejects a frame whose `reserved` word is not zero, and
     * stops at an all-zero header). Vector state is out of scope for this port. */
    frame.uc.uc_mcontext.sc_extdesc.reserved = 0;
    frame.uc.uc_mcontext.sc_extdesc.hdr = (struct riscv_ctx_hdr){0};

    pal_arch_sigreturn_frame(&frame);
}
