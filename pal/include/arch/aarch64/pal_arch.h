/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/*
 * This file contains definition of aarch64-specific aspects of PAL.
 */

#ifndef INSIDE_PAL_H
// TODO: fix this
#error This header is usable only inside pal.h (due to a cyclic dependency).
#endif

#pragma once

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>

#include "api.h"
#include "cpu.h"
#include "pal.h"

typedef struct pal_tcb PAL_TCB;

#define PAL_LIBOS_TCB_SIZE 256


#define STACK_PROTECTOR_CANARY_DEFAULT  0xbadbadbadbadUL

typedef struct pal_tcb {
    struct pal_tcb* self;
    /* random per-thread canary used by GCC's stack protector. On x86_64 this must sit at a fixed
     * %gs-relative offset because the compiler emits a direct %gs:[0x8] read on every function
     * entry; aarch64 (Milestone A stopgap, see pal_get_tcb() below) reaches PAL_TCB through a
     * plain global rather than a register, so the stack protector here just uses the generic
     * per-TU global-canary scheme (see top-level meson.build) and this field's offset isn't
     * compiler-enforced the way x86_64's is -- kept at the same offset anyway for structural
     * parity with x86_64 and so Milestone B (which *will* need a register-relative scheme once
     * it has somewhere to put one) doesn't have to reshuffle this struct. */
    uint64_t stack_protector_canary;
    /* uint64_t for alignment */
    uint64_t libos_tcb[(PAL_LIBOS_TCB_SIZE + sizeof(uint64_t) - 1) / sizeof(uint64_t)];
    /* data private to PAL implementation follows this struct. */
} PAL_TCB;

__attribute_no_stack_protector
static inline void pal_tcb_arch_set_stack_canary(PAL_TCB* tcb, uint64_t canary) {
    tcb->stack_protector_canary = canary;
}

static_assert(offsetof(PAL_TCB, stack_protector_canary) == 0x8,
              "unexpected offset of stack_protector_canary in PAL_TCB struct");

/*
 * Milestone A stopgap (single-threaded only): x86_64 reaches PAL_TCB via %gs, a register the
 * real ABI never assigns to anything else, so the app's own libc can't collide with it.
 * aarch64 has no such spare register -- TPIDR_EL0 is the one EL0-writable register, and the
 * real ABI requires it to hold the application's own libc TLS pointer (errno, locale data,
 * etc. all depend on it, even in trivial programs), and unlike x86_64's `arch_prctl`, setting
 * it is a plain unprivileged `msr`, not a syscall Saguaro could intercept. So for now PAL_TCB is
 * a single process-wide global (`g_pal_tcb_singleton`, defined in
 * pal/src/host/linux/arch/aarch64/pal_arch_exception.c) instead of per-thread register state --
 * correct for exactly one thread, and explicitly not yet a multi-threaded design. Fixing this
 * properly means patching the libc build (musl, not glibc: see the Milestone A plan notes) to
 * reserve one pointer field inside its own TCB struct for Saguaro's use, populated at
 * thread-creation time and reached via TPIDR_EL0 plus that fixed offset.
 */
/*
 * "protected" visibility, not the implicit default: this symbol must stay in libpal.so's dynamic
 * symbol table (LibOS, in libsysdb.so, resolves it via a cross-object GOT lookup -- see the #else
 * branch of pal_get_tcb() below), but default visibility also tells the linker "another object
 * loaded earlier might override this definition," which makes it reject *any* direct (non-GOT)
 * relocation against it, even one from within libpal.so itself -- the exact "may bind externally"
 * error this ran into. "protected" keeps the export but tells the linker libpal.so's own
 * definition is final, which is what makes the IN_PAL branch below's direct adrp/:lo12:
 * addressing link at all.
 */
extern __attribute__((visibility("protected"))) PAL_TCB* g_pal_tcb_singleton;

/* Implemented by the vm/tdx unikernel PALs (PAL_HOST_VM builds, see pal/src/host/vm-common/
 * arch/aarch64/); declared unconditionally so the inline pal_get_tcb() below can branch on it.
 * Never referenced in Linux PAL or LibOS builds (the branch is dead code without -DPAL_HOST_VM). */
PAL_TCB* pal_get_tcb_vm(void);

#include "pal_host_arch.h"

/* Per-thread PAL_TCB registry: maps each thread's application TLS pointer (TPIDR_EL0) to its
 * own PAL_TCB. The table itself (g_pal_tcb_registry) is defined in libpal.so (see
 * pal_arch_exception.c, like g_pal_tcb_singleton below) and reached from libsysdb.so via GOT;
 * PAL code here uses it directly. `add` is called only from LibOS-side registration hooks
 * (set_tls()), `del` from `_PalThreadExit()` and the exec winner, `scan` from both objects'
 * pal_get_tcb() and from syscallas.S (GOT-based walk). See g_pal_tcb_registry's long comment in
 * pal_arch_exception.c for the design. */
/* Note: the *definitions* in pal_arch_exception.c carry visibility("protected") so they are
 * exported through libpal's version script, but these declarations deliberately do NOT: a
 * protected *function* declaration makes GCC emit direct (non-PLT) calls, which GNU ld refuses
 * to bind against another shared object ("undefined reference"). g_pal_tcb_registry (data) is
 * referenced only through explicit :got: relocations (syscallas.S, pal_get_tcb asm), for which
 * protected works and matches the g_pal_tcb_singleton pattern. */
void libos_tcb_registry_add(PAL_TCB* tcb, uintptr_t guest_tp);
/* Deletes the binding only if it still belongs to `expected_tcb` -- see the definition's
 * ownership-check comment (TLS addresses get legitimately re-registered by new threads). */
void libos_tcb_registry_del(uintptr_t guest_tp, PAL_TCB* expected_tcb);
bool libos_tcb_registry_scan(PAL_TCB** out_tcb);

/*
 * Every thread's own TPIDR_EL0 register is independent architectural state (not shared memory),
 * so as long as a thread's own TPIDR_EL0 still holds what pal_set_tcb() put there (tagged with
 * bit 0 set -- real PAL_TCB pointers are always at least 8-byte aligned, so that bit is otherwise
 * always 0), reading it back directly gives that exact thread's own PAL_TCB with no cross-thread
 * interference at all. That covers Saguaro's own internal threads (the IPC worker, the async
 * worker) completely: they never run application code, so nothing else ever touches their
 * TPIDR_EL0 after pal_set_tcb() sets it.
 *
 * The one thread this *doesn't* cover on its own is whichever thread actually ends up running
 * loaded application code: aarch64 has no second EL0-writable register the way x86_64 has %gs
 * free alongside %fs (see the long comment on g_pal_tcb_singleton below), so that thread's own
 * libc TLS setup (ld.so's startup, or set_tls() when emulating the app's own clone()) overwrites
 * TPIDR_EL0 with its own (untagged) value once the app starts running. From that point on, this
 * thread's PAL_TCB has to come from somewhere else -- g_pal_tcb_singleton, snapshotted once via
 * pal_tcb_singleton_snapshot_before_user_entry() (see libos_rtld.c) in the narrow window after
 * this thread's own setup is done but before application code (and thus its TLS takeover) has
 * run. This is still exactly the single-threaded-*application* stopgap the aarch64 port
 * documents elsewhere: the fallback only ever needs to be correct for the one thread that enters
 * the application, same limitation as before, just no longer broken by Saguaro's own unrelated
 * internal threads along the way.
 */
static inline bool pal_tcb_from_tagged_tpidr(PAL_TCB** out_tcb) {
    uint64_t tp;
    __asm__("mrs %0, tpidr_el0" : "=r"(tp));
    if (tp & 0x1) {
        *out_tcb = (PAL_TCB*)(tp & ~(uint64_t)0x1);
        return true;
    }
    return false;
}

#ifdef IN_PAL
/*
 * Direct (non-GOT) addressing for the g_pal_tcb_singleton fallback read. This compiles as part of
 * libpal.so itself, the same object that defines g_pal_tcb_singleton, so the adrp/:lo12: pair is
 * a link-time-fixed PC-relative offset needing no runtime relocation at all -- unlike the #else
 * branch below (LibOS code, compiled into libsysdb.so, a genuinely different object), libpal.so
 * is executed directly by the kernel with no PT_INTERP/dynamic linker (see pal_start.S's
 * ENTRY(pal_start)) and so never gets a dynamic linker to populate its GOT. Plain C code
 * referencing this exported-visibility symbol would have the compiler conservatively emit
 * GOT-indirect loads anyway (to allow for symbol interposition), which is exactly what crashed
 * here originally: the GOT slot is never populated, so a GOT-indirect read returns NULL/garbage.
 * Hand-written direct addressing sidesteps that compiler policy entirely.
 */
static inline PAL_TCB* pal_get_tcb(void) {
#ifdef PAL_HOST_VM
    /*
     * Unikernel PAL (vm/tdx PAL_HOST_VM builds; Milestone B aarch64): there is no host OS, the
     * kernel itself maintains the "current thread PAL_TCB" exactly like x86_64 keeps it in the
     * GS base MSR -- on aarch64 that register role is played by a per-CPU slot maintained by
     * the context-switch asm (see vm-common/arch/aarch64/kernel_multicore.h `cur_tcb` and the
     * implementation note in vm-common/arch/aarch64/pal_common_misc.c). The tagged-TPIDR and
     * registry paths below belong to the *Linux* PAL design, where PAL code shares EL0 with
     * the app and TPIDR_EL0 is the app's own TLS register; in the unikernel this call runs at
     * EL1 inside syscall windows, where TPIDR_EL0 is by contract the *application* thread's
     * TLS and says nothing about PAL state.
     *
     * Two sides of the same truth: inside the kernel (IN_PAL) the authoritative slot is
     * per_cpu_data->cur_tcb, reached via TPIDR_EL1 (`pal_get_tcb_vm()`, EL1-only). The EL0
     * side reads the very same cell through TPIDRRO_EL0 -- an EL1-writable, EL0-read-only
     * register (exactly how Linux's vDSO exposes the cpu id) preloaded per CPU at boot with
     * `&per_cpu_data[cpu].cur_tcb`, which the context-switch asm writes on every switch.
     * One `mrs`+`ldr` always yields *this CPU's* running thread: the closest aarch64
     * equivalent of x86_64's ring-3-visible `%gs:` addressing, correct on any number of
     * CPUs. (The historical single global `g_pal_tcb_singleton` cell served that role while
     * the kernel was single-CPU; a second CPU could stomp it with its own thread's TCB --
     * or NULL while parking an AP boot context -- poisoning every other CPU's
     * pal_get_tcb(), which is what Milestone-B SMP brought down.)
     */
#ifdef IN_PAL
    return pal_get_tcb_vm();
#else
    PAL_TCB** cell;
    __asm__ volatile("mrs %0, tpidrro_el0" : "=r"(cell));
    return *cell;
#endif
#else
    PAL_TCB* tcb;
    if (pal_tcb_from_tagged_tpidr(&tcb)) {
        return tcb;
    }
    /* Application host thread (TPIDR_EL0 is the app's own TLS pointer): resolve this thread's own
     * PAL_TCB from the registry. Returning the singleton here (the pre-registry behavior) was
     * wrong for any thread but the main one -- e.g. `_PalThreadExit()` then recycled the *main*
     * thread's host stack slot. A miss is the main thread (pre-registration window) or a thread
     * mid-execve after the snapshot; the singleton fallback is correct for exactly those. No lazy
     * self-registration here: PAL code must not mutate the registry for app threads (LibOS's
     * pal_get_tcb() below owns that) -- e.g. `_PalThreadExit()` deliberately runs after its own
     * `libos_tcb_registry_del()`. */
    if (libos_tcb_registry_scan(&tcb)) {
        return tcb;
    }
    __asm__("adrp %0, g_pal_tcb_singleton\n"
            "ldr  %0, [%0, :lo12:g_pal_tcb_singleton]\n"
            : "=r"(tcb));
    return tcb;
#endif
}
#else
/*
 * Cross-object (LibOS, compiled into libsysdb.so) access. First try the per-thread PAL_TCB
 * registry (aarch64 multi-threading, see g_pal_tcb_registry's comment in libos_thread.c): the
 * thread's application TLS pointer (TPIDR_EL0, installed by set_tls() for clone/fork children --
 * which register themselves right there) keys its own PAL_TCB. A scan miss can only be the main
 * thread -- the one thread whose TLS pointer its own libc (ld.so's TLS_INIT_TP, or static
 * startup's) plants directly, with no Saguaro hook to register it -- so for the miss path the
 * g_pal_tcb_singleton fallback *is* the correct answer, and we lazily self-register the main
 * thread so later scans hit (idempotent: same key + same TCB; concurrent first syscalls from
 * several never-registered threads cannot occur for the reasons above). The GOT-indirect read of
 * g_pal_tcb_singleton itself (libsysdb and libpal are different shared objects, see the #else
 * branch above) is kept as the final fallback for the pre-registration startup window.
 */
static inline PAL_TCB* pal_get_tcb(void) {
    PAL_TCB* tcb;
    if (pal_tcb_from_tagged_tpidr(&tcb)) {
        return tcb;
    }
    if (libos_tcb_registry_scan(&tcb)) {
        return tcb;
    }
    PAL_TCB** got_slot;
    __asm__("adrp %0, :got:g_pal_tcb_singleton\n"
            "ldr  %0, [%0, :got_lo12:g_pal_tcb_singleton]\n"
            : "=r"(got_slot));
    tcb = *got_slot;
    if (tcb) {
        uintptr_t tp;
        __asm__("mrs %0, tpidr_el0" : "=r"(tp));
        libos_tcb_registry_add(tcb, tp);
    }
    return tcb;
}
#endif

/* Called exactly once, by LibOS, in the narrow window after the thread that is about to start
 * running application code has its own PAL_TCB set up (via pal_set_tcb(), reachable at that
 * point through the tagged TPIDR_EL0 fast path above) but before that thread jumps into
 * application entry code -- which, via ld.so's own startup or set_tls() emulating the
 * application's own clone(), is about to overwrite TPIDR_EL0 with the application's own libc TLS
 * pointer. g_pal_tcb_singleton is this thread's only remaining way to reach its own PAL_TCB after
 * that happens (see the #else branch of pal_get_tcb() above), so it has to be snapshotted now,
 * while the tagged fast path can still supply the right value. Needs the same IN_PAL/#else
 * addressing-mode split as pal_get_tcb() itself, since this (unlike pal_set_tcb(), PAL-only)
 * is called from LibOS. */
static inline void pal_tcb_singleton_snapshot_before_user_entry(void) {
    PAL_TCB* tcb = pal_get_tcb();
#ifdef IN_PAL
    __asm__("adrp x9, g_pal_tcb_singleton\n"
            "str  %0, [x9, :lo12:g_pal_tcb_singleton]\n"
            :
            : "r"(tcb)
            : "x9");
#else
    PAL_TCB** got_slot;
    __asm__("adrp x9, :got:g_pal_tcb_singleton\n"
            "ldr  x9, [x9, :got_lo12:g_pal_tcb_singleton]\n"
            "mov  %0, x9\n"
            : "=r"(got_slot)
            :
            : "x9");
    *got_slot = tcb;
#endif
}

/* Called by _PalThreadExit() as the last moment this thread's own PAL_TCB is needed (it was
 * resolved through the registry just before): drop the thread's registry entry, which is keyed by
 * its app TLS pointer. From then on the abandoned TLS address may legitimately be reused by
 * another thread's registration; a stale entry would let that thread hijack this one's (soon
 * freed) PAL_TCB. The deletion is ownership-checked against \p tcb: a new thread born after the
 * region's mmap reuse may already have re-registered this key, and that binding must survive.
 * Tagged (Saguaro-internal) threads have no registry entry, so they are skipped. */
static inline void pal_arch_thread_exit_release_tcb(PAL_TCB* tcb) {
    uintptr_t exit_tp;
    __asm__("mrs %0, tpidr_el0" : "=r"(exit_tp));
    if (!(exit_tp & 1)) {
        libos_tcb_registry_del(exit_tp, tcb);
    }
}

static inline unsigned long count_ulong_bits_set(unsigned long x) {
    return (unsigned long)__builtin_popcountl(x);
}

/* holds /proc/cpuinfo data; on aarch64 these are filled from MIDR_EL1 (see pal_cpuid.c), not a
 * CPUID-leaf vendor/family/model/stepping scheme, but the field names/types are kept identical
 * to x86_64's so generic code (pal_main.c, libos/src/fs/proc/info.c) doesn't need to care. */
struct pal_cpu_info {
    const char* cpu_vendor;
    const char* cpu_brand;
    uint64_t cpu_family;
    uint64_t cpu_model;
    uint64_t cpu_stepping;
    double cpu_bogomips;
    /* AT_HWCAP / AT_HWCAP2 that the LibOS gives applications: the PAL's view of the CPU
     * (_PalGetHwcaps() of each host PAL), masked to PAL_HWCAP_ALLOWED / PAL_HWCAP2_ALLOWED
     * (pal_hwcap.h) in pal_cpuid.c; see dev docs: aarch64/hwcap/PLAN.md */
    uint64_t cpu_hwcap;
    uint64_t cpu_hwcap2;
};

/* aarch64 has no segment registers; nothing stands in for x86_64's `union pal_csgsfs`. */

/*
 * aarch64 FP/SIMD context, i.e. the analog of x86_64's FXSAVE/XSAVE-derived PAL_FPSTATE.
 *
 * This must be byte-for-byte identical to the real kernel `struct fpsimd_context`
 * (`struct _aarch64_ctx head` flattened into `magic`+`size`, then FPSR/FPCR, then 32 128-bit V
 * registers) -- unlike x86_64, where PAL_FPSTATE is a from-scratch struct that PAL's own
 * exception machinery fully owns, aarch64's `context->fpregs` (see ucontext_to_pal_context() in
 * linux/ucontext.h) is a pointer taken directly from inside the real kernel-delivered sigcontext
 * record, so this layout is dictated by the kernel ABI, not a Saguaro design choice. There is no
 * magic-number/feature-bitmap validation scheme the way x86_64's XSAVE area has one (the
 * magic/size pair here just identifies which kind of record this is within the kernel's
 * extensible `__reserved[]` chain), and (for now, scope of the aarch64 port) no SVE/SVE2
 * extended-register state.
 */
typedef struct {
    uint32_t magic; /* FPSIMD_MAGIC, see linux/ucontext.h */
    uint32_t size;  /* sizeof(struct fpsimd_context) */
    uint32_t fpsr;
    uint32_t fpcr;
    __uint128_t vregs[32];
} PAL_FPSTATE;

#define PAL_XSTATE_ALIGN 16

typedef struct {
    PAL_FPSTATE fpstate;
} __attribute__((packed, aligned(PAL_XSTATE_ALIGN))) PAL_XREGS_STATE;

/*
 * Define PAL_CONTEXT outside the typedef for Doxygen.
 *
 * `regs`/`sp`/`pc`/`pstate` deliberately mirror the layout of the real Linux aarch64
 * `struct user_pt_regs` / `mcontext_t.regs[]` (regs[0..30] == x0..x30), rather than inventing
 * named GPR fields the way the x86_64 PAL_CONTEXT does -- this is the kernel's own convention on
 * this architecture, not a Saguaro-specific choice.
 */
struct PAL_CONTEXT {
    uint64_t regs[31]; /* x0..x30; x8 carries the syscall number on entry, x0 the return value */
    uint64_t sp;
    uint64_t pc;
    uint64_t pstate;
    uint64_t fault_address; /* FAR_EL1-sourced; aarch64 analog of x86_64's cr2 */
    uint64_t esr;           /* ESR_EL1-sourced exception syndrome; analog of trapno/err */
    uint64_t oldmask;

    PAL_XREGS_STATE* fpregs;

    uint32_t fpsr;
    uint32_t fpcr;
    uint8_t is_fpregs_used; /* Equal to 0 iff `fpregs` is not populated. */
    uint8_t _pad[3];

    /* Snapshot of the first syscall argument, saved by do_syscall() before dispatch. aarch64
     * (like the arm64 kernel's pt_regs->orig_x0, which exists for exactly this reason) delivers
     * the syscall return value in the same register as arg0, so a syscall answering one of the
     * -ERESTART* codes has already destroyed its first argument by the time restart_syscall()
     * re-dispatches it; restart_syscall() restores x0 from this mirror. Not copied to/from
     * guest signal frames (pal_context_to_ucontext()/ucontext_to_pal_context() ignore it):
     * after rt_sigreturn the syscall re-runs from the syscall-entry point and do_syscall()
     * re-saves this field fresh. */
    uint64_t orig_x0;
};
typedef struct PAL_CONTEXT PAL_CONTEXT;

/*
 * Exact context restore (see libos/src/arch/aarch64/syscallas.S, _return_from_syscall).
 *
 * A thread interrupted by an asynchronous signal (or a fault) can be at any instruction, including
 * ones where x16/x17 (IP0/IP1, live inside PLT stubs and veneers) or the condition flags are live.
 * Returning to such a context from user space needs a scratch register for the final jump, so the
 * ordinary syscall-return path (which treats x17 as scratch, as the ABI allows after a call) cannot
 * be used. Instead the LibOS executes `brk #PAL_BRK_RESTORE_CONTEXT` with x0 = the PAL_CONTEXT, and
 * the PAL's SIGTRAP handler overwrites the host ucontext with it, so the kernel restores every
 * register and the flags exactly, as it does for any signal frame.
 *
 * Contexts that came from the host (ucontext_to_pal_context()) are tagged with
 * PAL_PSTATE_EXACT_CONTEXT in `pstate` (bit 62 is RES0 in the real PSTATE, and is cleared again by
 * the kernel on return); contexts built by the syscall-entry path have pstate == 0 and keep the
 * fast path.
 */
#define PAL_PSTATE_EXACT_CONTEXT (1ULL << 62)
#define PAL_BRK_RESTORE_CONTEXT  0x6772 /* "gr" */
#define PAL_HAS_CONTEXT_RESTORE_TRAP 1

typedef int64_t arch_syscall_arg_t;

#define ALL_SYSCALL_ARGS(context) \
    (context)->regs[0], \
    (context)->regs[1], \
    (context)->regs[2], \
    (context)->regs[3], \
    (context)->regs[4], \
    (context)->regs[5]


static inline void pal_context_set_ip(PAL_CONTEXT* context, uintptr_t insnptr) {
    context->pc = insnptr;
}
static inline uintptr_t pal_context_get_ip(PAL_CONTEXT* context) {
    return context->pc;
}

static inline void pal_context_set_sp(PAL_CONTEXT* context, uintptr_t sp) {
    context->sp = sp;
}
static inline uintptr_t pal_context_get_sp(PAL_CONTEXT* context) {
    return context->sp;
}

static inline void pal_context_set_retval(PAL_CONTEXT* context, uint64_t val) {
    context->regs[0] = val;
}
static inline uint64_t pal_context_get_retval(PAL_CONTEXT* context) {
    return context->regs[0];
}

static inline uint64_t pal_context_get_syscall(PAL_CONTEXT* context) {
    /* Unlike x86_64 (where RAX serves as both the syscall number on entry and the return value on
     * exit), aarch64 keeps the syscall number in X8 throughout, separate from the X0 return
     * value. */
    return context->regs[8];
}

/* Copies `PAL_CONTEXT` without extended FP/SIMD state (but keeping control words). */
static inline void pal_context_copy(PAL_CONTEXT* dst, PAL_CONTEXT* src) {
    *dst = *src;
    dst->is_fpregs_used = 0;
    dst->fpregs = NULL;
    if (src->is_fpregs_used) {
        dst->fpsr = src->fpregs->fpstate.fpsr;
        dst->fpcr = src->fpregs->fpstate.fpcr;
    }
}
