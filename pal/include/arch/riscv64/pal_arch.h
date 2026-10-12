/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/*
 * This file contains definition of riscv64-specific aspects of PAL.
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
     * entry; riscv64 (same situation as aarch64: the thread pointer `tp` is architectural state
     * the *application's* libc owns -- there is no second EL0-writable register, and unlike
     * x86_64's arch_prctl(ARCH_SET_FS), `tp` is set by plain unprivileged code the PAL cannot
     * intercept) reaches PAL_TCB through the per-thread registry below rather than a register, so
     * the stack protector uses the generic (per-TU global canary) scheme (see top-level
     * meson.build) and this field's offset isn't compiler-enforced the way x86_64's is -- kept at
     * the same offset anyway for structural parity with x86_64. */
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
 * Same design as the aarch64 port (see findings #9, #12 and #13 in
 * Documentation/devel/aarch64/direct/FINDINGS.md): the thread pointer belongs to the
 * application's libc, so PAL_TCB is reached through (in order) a tagged-`tp` fast path for
 * Gramine's own internal threads, the per-thread PAL_TCB registry keyed by the application TLS
 * pointer, and finally this process-wide global `g_pal_tcb_singleton` -- the correct fallback for
 * exactly the one thread that runs the application (whose own libc plants `tp` with no Gramine
 * hook to register it).
 */
/*
 * "protected" visibility, not the implicit default: this symbol must stay in libpal.so's dynamic
 * symbol table (LibOS, in libsysdb.so, resolves it via a cross-object GOT lookup -- see the #else
 * branch of pal_get_tcb() below), but default visibility also tells the linker "another object
 * loaded earlier might override this definition," which makes it reject *any* direct (non-GOT)
 * relocation against it, even one from within libpal.so itself. "protected" keeps the export but
 * tells the linker libpal.so's own definition is final, which is what makes the IN_PAL branch
 * below's direct auipc/:lo12: addressing link at all. (RISC-V note: this is also why the PAL is a
 * shared object like on the other architectures -- gp-relative relaxation cannot apply to
 * preemptible symbols in an ET_DYN, so the PAL's earliest code never depends on `gp`.)
 */
extern __attribute__((visibility("protected"))) PAL_TCB* g_pal_tcb_singleton;

/* Implemented by the vm/tdx unikernel PALs (PAL_HOST_VM builds); declared unconditionally so the
 * inline pal_get_tcb() below can branch on it. Never referenced in Linux PAL or LibOS builds (the
 * branch is dead code without -DPAL_HOST_VM). A riscv64 vm PAL does not exist yet. */
PAL_TCB* pal_get_tcb_vm(void);

#include "pal_host_arch.h"

/* Per-thread PAL_TCB registry: maps each thread's application TLS pointer (`tp`) to its own
 * PAL_TCB. The table itself (g_pal_tcb_registry) is defined in libpal.so (see
 * pal_arch_exception.c, like g_pal_tcb_singleton above) and reached from libsysdb.so via GOT;
 * PAL code here uses it directly. `add` is called only from LibOS-side registration hooks
 * (set_tls()), `del` from `_PalThreadExit()` and the exec winner, `scan` from both objects'
 * pal_get_tcb() and from syscallas.S (GOT-based walk). See g_pal_tcb_registry's long comment in
 * pal_arch_exception.c for the design. */
/* Note: the *definitions* in pal_arch_exception.c carry visibility("protected") so they are
 * exported through libpal's version script, but these declarations deliberately do NOT: a
 * protected *function* declaration makes GCC emit direct (non-PLT) calls, which GNU ld refuses
 * to bind against another shared object ("undefined reference"). g_pal_tcb_registry (data) is
 * referenced only through GOT loads (`la` of the dynamic symbol from libsysdb.so compiles to the
 * GOT-load pair; syscallas.S does the same), which works and matches the g_pal_tcb_singleton
 * pattern. */
void libos_tcb_registry_add(PAL_TCB* tcb, uintptr_t guest_tp);
/* Deletes the binding only if it still belongs to `expected_tcb` -- see the definition's
 * ownership-check comment (TLS addresses get legitimately re-registered by new threads). */
void libos_tcb_registry_del(uintptr_t guest_tp, PAL_TCB* expected_tcb);
bool libos_tcb_registry_scan(PAL_TCB** out_tcb);

/*
 * Every thread's own `tp` register is independent architectural state (not shared memory), so as
 * long as a thread's own `tp` still holds what pal_set_tcb() put there (tagged with bit 0 set --
 * real PAL_TCB pointers are always at least 8-byte aligned, so that bit is otherwise always 0),
 * reading it back directly gives that exact thread's own PAL_TCB with no cross-thread
 * interference at all. That covers Gramine's own internal threads (the IPC worker, the async
 * worker) completely: they never run application code, so nothing else ever touches their `tp`
 * after pal_set_tcb() sets it.
 *
 * The one thread this *doesn't* cover is whichever thread actually ends up running loaded
 * application code: riscv64 has no second user-writable TLS register the way x86_64 has %gs free
 * alongside %fs, so that thread's own libc TLS setup (ld.so's startup, or set_tls() when
 * emulating the app's own clone()) overwrites `tp` with its own (untagged) value once the app
 * starts running. From that point on, this thread's PAL_TCB comes from g_pal_tcb_singleton,
 * snapshotted once via pal_tcb_singleton_snapshot_before_user_entry() in the narrow window after
 * this thread's own setup is done but before application code (and thus its TLS takeover) has run.
 */
static inline bool pal_tcb_from_tagged_tp(PAL_TCB** out_tcb) {
    uint64_t tp;
    __asm__("mv %0, tp" : "=r"(tp));
    if (tp & 0x1) {
        *out_tcb = (PAL_TCB*)(tp & ~(uint64_t)0x1);
        return true;
    }
    return false;
}

#ifdef IN_PAL
/*
 * Direct (non-GOT) addressing for the g_pal_tcb_singleton fallback read. This compiles as part of
 * libpal.so itself, the same object that defines g_pal_tcb_singleton, so the `lla` expansion is
 * a link-time-fixed PC-relative offset needing no runtime relocation at all -- unlike the #else
 * branch below (LibOS code, compiled into libsysdb.so, a genuinely different object), libpal.so
 * is executed directly by the kernel with no PT_INTERP/dynamic linker (see pal_start.S's
 * ENTRY(pal_start)) and so never gets a dynamic linker to populate its GOT. Plain C code
 * referencing this exported-visibility symbol would have the compiler conservatively emit
 * GOT-indirect loads anyway (to allow for symbol interposition), which is exactly what crashed
 * the aarch64 bring-up here originally: the GOT slot is never populated, so a GOT-indirect read
 * returns NULL/garbage. Hand-written direct addressing sidesteps that compiler policy entirely.
 *
 * `gp` is deliberately not involved anywhere in this path: gp-relative relaxation only applies to
 * non-preemptible symbols in non-PIE executables, and libpal.so is an ET_DYN (see the
 * g_pal_tcb_singleton comment above), so no access in it is ever gp-relative.
 */
static inline PAL_TCB* pal_get_tcb(void) {
#ifdef PAL_HOST_VM
    /* Unikernel PAL (PAL_HOST_VM builds): a riscv64 vm PAL does not exist yet (out of scope, see
     * Documentation/devel/riscv/PLAN.md); this branch exists only for structural parity with the
     * aarch64 header and is dead code in every riscv64 build we produce. */
    return pal_get_tcb_vm();
#else
    PAL_TCB* tcb;
    if (pal_tcb_from_tagged_tp(&tcb)) {
        return tcb;
    }
    /* Application host thread (`tp` is the app's own TLS pointer): resolve this thread's own
     * PAL_TCB from the registry. Returning the singleton here (the pre-registry behavior) was
     * wrong for any thread but the main one. A miss is the main thread (pre-registration window)
     * or a thread mid-execve after the snapshot; the singleton fallback is correct for exactly
     * those. No lazy self-registration here: PAL code must not mutate the registry for app
     * threads (LibOS's pal_get_tcb() below owns that). */
    if (libos_tcb_registry_scan(&tcb)) {
        return tcb;
    }
    /* Direct (GOT-free) read of the variable via gas's `lla` pseudo-op: for a symbol resolved
     * within one link it expands to a link-time-fixed auipc+addi pair needing no runtime
     * relocation at all -- the riscv64 analog of the aarch64 sibling's hand-written direct-adrp
     * trick (see the long comment above; note that plain C would compile to a GOT-indirect
     * load, whose slot is only populated once perform_relocations() has run). It *must* be the
     * pseudo-op rather than a hand-written %pcrel_hi/%pcrel_lo pair: GNU ld matches a
     * R_RISCV_PCREL_LO12 against its PCREL_HI20 only through the R_RISCV_RELAX markers gas
     * emits around its own expansions (psabi-doc issue #90; a hand-written pair links under
     * gas but fails with "dangerous relocation: %pcrel_lo missing matching %pcrel_hi"). */
    __asm__("lla  %0, g_pal_tcb_singleton\n"
            "ld   %0, 0(%0)\n"
            : "=r"(tcb));
    return tcb;
#endif
}
#else
/*
 * Cross-object (LibOS, compiled into libsysdb.so) access. First try the per-thread PAL_TCB
 * registry: the thread's application TLS pointer (`tp`, installed by set_tls() for clone/fork
 * children -- which register themselves right there) keys its own PAL_TCB. A scan miss can only
 * be the main thread -- the one thread whose TLS pointer its own libc (ld.so's TLS setup or
 * static startup's) plants directly, with no Gramine hook to register it -- so for the miss path
 * the g_pal_tcb_singleton fallback *is* the correct answer, and we lazily self-register the main
 * thread so later scans hit (idempotent: same key + same TCB; concurrent first syscalls from
 * several never-registered threads cannot occur for the reasons above). The GOT-indirect read of
 * g_pal_tcb_singleton itself (libsysdb and libpal are different shared objects) is kept as the
 * final fallback for the pre-registration startup window.
 */
static inline PAL_TCB* pal_get_tcb(void) {
    PAL_TCB* tcb;
    if (pal_tcb_from_tagged_tp(&tcb)) {
        return tcb;
    }
    if (libos_tcb_registry_scan(&tcb)) {
        return tcb;
    }
    /* GOT-indirect read of g_pal_tcb_singleton (libsysdb and libpal are different shared
     * objects): `la` of a dynamic symbol assembles to the GOT-load pair and the slot is
     * populated by the PAL's own ELF loader (pal_rtld) when libsysdb.so is mapped. Lazily
     * self-register the main thread here (finding #9's scheme on aarch64: the scan miss *is*
     * the main thread, idempotently), so later scans hit the registry. */
    __asm__("la   %0, g_pal_tcb_singleton\n"
            "ld   %0, 0(%0)\n"
            : "=&r"(tcb)
            :
            : "memory");
    if (tcb) {
        uintptr_t tp;
        __asm__("mv %0, tp" : "=r"(tp));
        libos_tcb_registry_add(tcb, tp);
    }
    return tcb;
}
#endif

/* Called exactly once, by LibOS, in the narrow window after the thread that is about to start
 * running application code has its own PAL_TCB set up (via pal_set_tcb(), reachable at that point
 * through the tagged-`tp` fast path above) but before that thread jumps into application entry
 * code -- which, via ld.so's own startup or set_tls() emulating the application's own clone(), is
 * about to overwrite `tp` with the application's own libc TLS pointer. g_pal_tcb_singleton is
 * this thread's only remaining way to reach its own PAL_TCB after that happens, so it has to be
 * snapshotted now, while the tagged fast path can still supply the right value. Needs the same
 * IN_PAL/#else addressing-mode split as pal_get_tcb() itself. */
static inline void pal_tcb_singleton_snapshot_before_user_entry(void) {
    PAL_TCB* tcb = pal_get_tcb();
#ifdef IN_PAL
    __asm__ volatile("lla  t0, g_pal_tcb_singleton\n"
                     "sd   %0, 0(t0)\n"
                     :
                     : "r"(tcb)
                     : "t0");
#else
    /* GOT-based load of the variable's address (same `la` GOT-load as pal_get_tcb()'s #else
     * branch; the GOT slot is libsysdb's view of libpal's variable), then an ordinary store
     * through it -- writes the variable itself, like aarch64's `adrp :got: + ldr + str`. */
    uintptr_t addr;
    __asm__("la %0, g_pal_tcb_singleton\n" : "=&r"(addr) : : "memory");
    *(PAL_TCB**)addr = tcb;
#endif
}

/* Called by _PalThreadExit() as the last moment this thread's own PAL_TCB is needed (it was
 * resolved through the registry just before): drop the thread's registry entry, which is keyed by
 * its app TLS pointer. Tagged (Gramine-internal) threads have no registry entry, so they are
 * skipped. */
static inline void pal_arch_thread_exit_release_tcb(PAL_TCB* tcb) {
    uintptr_t exit_tp;
    __asm__("mv %0, tp" : "=r"(exit_tp));
    if (!(exit_tp & 1)) {
        libos_tcb_registry_del(exit_tp, tcb);
    }
}

static inline unsigned long count_ulong_bits_set(unsigned long x) {
    return (unsigned long)__builtin_popcountl(x);
}

/* holds /proc/cpuinfo data; on riscv64 these are filled from the host's per-hart /proc/cpuinfo
 * `uarch`/`isa`/`mmu`/identity-CSR fields (see pal_cpuid.c) -- riscv64 has no CPUID-leaf
 * vendor/family/model/stepping scheme, and the identity CSRs (mvendorid/marchid/mimpid) are
 * supervisor-only, so the first three x86-shaped fields carry that triplet instead (0 on CPUs
 * whose Device Tree carries no vendor info, e.g. QEMU `virt`), `cpu_brand` carries the `isa`
 * string, and `cpu_vendor` the Device Tree `uarch` name when the host kernel prints one. The
 * field names/types of the x86_64 record are kept as-is so generic code (pal_main.c, the LibOS
 * proc_fs code) doesn't need to care; `cpu_mmu` is the one *addition*, because the
 * riscv64 /proc/cpuinfo the LibOS renders must show the page-table mode (`sv39`/`sv48`/...),
 * which has no x86_64 analog to hide behind -- the struct is architecture-private by
 * construction (only the arch's own cpuid/cpuinfo code reads it), so this is free. */
struct pal_cpu_info {
    const char* cpu_vendor;
    const char* cpu_brand;
    const char* cpu_mmu;
    uint64_t cpu_family;
    uint64_t cpu_model;
    uint64_t cpu_stepping;
    double cpu_bogomips;
};

/* riscv64 has no segment registers; nothing stands in for x86_64's `union pal_csgsfs`. */

/*
 * riscv64 floating-point context, i.e. the analog of x86_64's FXSAVE/XSAVE-derived PAL_FPSTATE
 * and aarch64's fpsimd_context-derived one.
 *
 * Unlike both other architectures, the riscv64 signal ABI stores scalar F/D register state
 * *inline* in `struct sigcontext.sc_fpregs` with no magic-tagged record header of its own (see
 * linux/ucontext.h): the kernel writes `struct __riscv_d_ext_state` (32 64-bit f registers, then
 * the 32-bit fcsr) at that fixed offset whenever F/D are present, which on lp64d is always. Only
 * the *optional* vector state uses the extensible `__riscv_ctx_hdr` {magic, size} scheme
 * (RISCV_V_MAGIC record appended after it). So PAL_FPSTATE here is a plain mirror of the d-state
 * -- dictated by the kernel ABI, not a Gramine design choice -- and there is no record header to
 * carry. Vector state is out of scope for now (see Documentation/devel/riscv/PLAN.md decisions);
 * when it is added, `context->fpregs` keeps aliasing the d-state and the V record rides along
 * untouched inside the frame.
 */
typedef struct {
    uint64_t f[32]; /* f0..f31; the kernel always saves the full 64-bit (D) width on lp64d */
    uint32_t fcsr;  /* FXCR-equivalent control: FRM (rounding) + FFLAGS (exceptions) */
} PAL_FPSTATE;

#define PAL_XSTATE_ALIGN 16

typedef struct {
    PAL_FPSTATE fpstate;
} __attribute__((packed, aligned(PAL_XSTATE_ALIGN))) PAL_XREGS_STATE;

/*
 * Define PAL_CONTEXT outside the typedef for Doxygen.
 *
 * `regs` is indexed by the *RISC-V ABI register numbers* (regs[0]..regs[31] == x0..x31, i.e.
 * regs[1]=ra, regs[2]=sp, regs[10..17]=a0..a7, regs[8]=fp/s0), which differs from the riscv64
 * kernel signal frame's own field order (pc, ra, sp, gp, tp, t0..t2, s0..s1, a0..a7, s2..s11,
 * t3..t6 -- see struct user_regs_struct in linux/ucontext.h, converted field by field). Unlike
 * aarch64's regs[31]+sp+pc layout, sp lives in regs[2] (riscv has no separate stack-pointer
 * save/restore instruction to justify carving it out), and `pc`/`pstate` stay as separate fields
 * for shape parity with the other architectures: `pstate` has no architectural counterpart on
 * riscv64 (there are no condition-code flags) and exists purely as a Gramine-internal tag holder
 * (see PAL_PSTATE_EXACT_CONTEXT below).
 */
struct PAL_CONTEXT {
    uint64_t regs[32]; /* x0..x31; x0 is architecturally zero, regs[10] carries the return value,
                          regs[17] the syscall number on entry */
    uint64_t pc;
    uint64_t pstate;   /* Gramine-internal flags only (no riscv64 condition flags exist) */
    uint64_t fault_address; /* from siginfo's si_addr; riscv64 sigcontext carries no faulting
                               address (no FAR-equivalent), see handle_sync_signal's addr arg */
    uint64_t esr;           /* reserved, always 0: no exception-syndrome-register equivalent */
    uint64_t oldmask;

    PAL_XREGS_STATE* fpregs;

    uint32_t fcsr;
    uint8_t is_fpregs_used; /* Equal to 0 iff `fpregs` is not populated. */
    uint8_t _pad[3];

    /* Snapshot of the first syscall argument, saved by do_syscall() before dispatch. riscv64
     * (like aarch64, and like the riscv kernel's own answer to the same problem, except that
     * kernel keeps *all* GPRs in the frame so the need never arose there) delivers the syscall
     * return value in the same register as arg0 (a0), so a syscall answering one of the
     * -ERESTART* codes has already destroyed its first argument by the time restart_syscall()
     * re-dispatches it; restart_syscall() restores a0 from this mirror. Not copied to/from guest
     * signal frames (pal_context_to_ucontext()/ucontext_to_pal_context() ignore it): after
     * rt_sigreturn the syscall re-runs from the syscall-entry point and do_syscall() re-saves
     * this field fresh. */
    uint64_t orig_a0;
};
typedef struct PAL_CONTEXT PAL_CONTEXT;

/*
 * Exact context restore (see libos/src/arch/riscv64/syscallas.S, _return_from_syscall).
 *
 * A thread interrupted by an asynchronous signal can be at any instruction, including ones where
 * ra (x1, the link register), gp/tp, or t0/t1 (the PLT-stub scratch pair) are live. RISC-V has no
 * condition-code flags (so the second half of aarch64 finding #15 cannot happen here), but the
 * register half applies verbatim: returning to a resumed instruction needs a scratch register for
 * the final jump, and any `jr rs` / register load clobbers app state that the ordinary
 * syscall-return path (which uses t1 as scratch, as the ABI allows after a call) cannot clobber.
 * Instead the LibOS executes `ebreak` with a0 = the PAL_CONTEXT and a7 = PAL_BRK_RESTORE_CONTEXT
 * (a distinctive 64-bit value that no compiler ever places in a7 at an app ebreak), and the
 * PAL's SIGTRAP handler overwrites the host ucontext with it, so the kernel's own sigreturn
 * restores every register exactly, as it does for any signal frame. The ebreak raises SIGTRAP
 * (the breakpoint exception), exactly like aarch64's `brk`.
 *
 * Contexts that came from the host (ucontext_to_pal_context()) are tagged with
 * PAL_PSTATE_EXACT_CONTEXT in `pstate` (a Gramine-only field on riscv64, so no RES0 games needed);
 * contexts built by the syscall-entry path have pstate == 0 and keep the fast path.
 */
#define PAL_PSTATE_EXACT_CONTEXT (1ULL << 62)
#define PAL_BRK_RESTORE_CONTEXT  0x6772677267726772ULL /* "grgrgrgr" -- a7 payload of the exact
                                                          restore ebreak, see pal_arch_handle_trap
                                                          () and the ebreak immediate-less
                                                          note in syscallas.S */
#define PAL_HAS_CONTEXT_RESTORE_TRAP 1
#define PAL_HAS_SYSCALL_TRAP_HOOK 1

/* Resumes `context` exactly through rt_sigreturn(); the LibOS's only way back to the application.
 * Defined in pal/src/host/linux/arch/riscv64/pal_arch_exception.c and exported from libpal.so. */
__attribute__((__noreturn__)) void pal_arch_restore_context(struct PAL_CONTEXT* context);

typedef int64_t arch_syscall_arg_t;

#define ALL_SYSCALL_ARGS(context) \
    (context)->regs[10], \
    (context)->regs[11], \
    (context)->regs[12], \
    (context)->regs[13], \
    (context)->regs[14], \
    (context)->regs[15]


static inline void pal_context_set_ip(PAL_CONTEXT* context, uintptr_t insnptr) {
    context->pc = insnptr;
}
static inline uintptr_t pal_context_get_ip(PAL_CONTEXT* context) {
    return context->pc;
}

static inline void pal_context_set_sp(PAL_CONTEXT* context, uintptr_t sp) {
    context->regs[2] = sp;
}
static inline uintptr_t pal_context_get_sp(PAL_CONTEXT* context) {
    return context->regs[2];
}

static inline void pal_context_set_retval(PAL_CONTEXT* context, uint64_t val) {
    context->regs[10] = val;
}
static inline uint64_t pal_context_get_retval(PAL_CONTEXT* context) {
    return context->regs[10];
}

static inline uint64_t pal_context_get_syscall(PAL_CONTEXT* context) {
    /* Like aarch64 (and unlike x86_64, where RAX conflates the syscall number on entry with the
     * return value on exit), riscv64 keeps the syscall number in a7, separate from the a0 return
     * value. */
    return context->regs[17];
}

/* Copies `PAL_CONTEXT` without extended FP state (but keeping the control word). */
static inline void pal_context_copy(PAL_CONTEXT* dst, PAL_CONTEXT* src) {
    *dst = *src;
    dst->is_fpregs_used = 0;
    dst->fpregs = NULL;
    if (src->is_fpregs_used) {
        dst->fcsr = src->fpregs->fpstate.fcsr;
    }
}
