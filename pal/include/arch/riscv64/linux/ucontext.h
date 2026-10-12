/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

#pragma once

#include <stdint.h>

#include "api.h"
#include "assert.h"
#include "pal.h"

/* Structure definitions source: Linux arch/riscv/include/uapi/asm/{ptrace.h,sigcontext.h} and
 * glibc's sys/ucontext.h layout for riscv64. Shape compared with the other ports: x86_64 aliases
 * a pointer-like `fpstate`, aarch64 scans a magic-tagged record chain inside a 4KB `__reserved`
 * area; riscv64 is the simplest of the three *for scalar state* -- all 32 GPRs plus pc/sp are
 * plain named fields of `struct user_regs_struct` (a fixed 256-byte block) and the F/D registers
 * are the fixed-size `struct __riscv_d_ext_state` (32 64-bit registers + a 32-bit fcsr) written
 * inline at `sc_fpregs`. There is no header record for F/D at all. The extensible-record scheme
 * (`__riscv_ctx_hdr` with RISCV_V_MAGIC) is used *only* by the optional vector extension: when
 * V is active the kernel overlays an `__riscv_extra_ext_header` whose trailing `hdr` sits past
 * the F/D block and appends the vector state record after the sigcontext. Scalar F/D state then
 * still lives at offset 0 of `sc_fpregs` (inside the overlay's __padding), which is what makes
 * the aliasing below V-transparent: the F/D block (264 bytes) never reaches the overlay's `hdr`
 * (at offset 520), so a V record present in a frame is neither parsed nor clobbered here.
 * (Vector support is out of scope for the riscv64 port for now, and the test guest's ISA does not
 * include V; see Documentation/devel/riscv/plan.md decisions.) */

#define RISCV_V_MAGIC 0x53465457 /* "SFTW" -- vector-extension state record magic */
#define END_MAGIC     0x0        /* terminator of the extension chain */

struct riscv_ctx_hdr {
    uint32_t magic;
    uint32_t size;
};

/* Must match struct user_regs_struct (arch/riscv include/uapi/asm/ptrace.h) field for field --
 * this is the kernel's own GPR save order (note: *not* x0..x31 numeric order; x0 is architectural
 * zero and absent). */
struct riscv_user_regs {
    uint64_t pc;
    uint64_t ra;
    uint64_t sp;
    uint64_t gp;
    uint64_t tp;
    uint64_t t0;
    uint64_t t1;
    uint64_t t2;
    uint64_t s0;
    uint64_t s1;
    uint64_t a0;
    uint64_t a1;
    uint64_t a2;
    uint64_t a3;
    uint64_t a4;
    uint64_t a5;
    uint64_t a6;
    uint64_t a7;
    uint64_t s2;
    uint64_t s3;
    uint64_t s4;
    uint64_t s5;
    uint64_t s6;
    uint64_t s7;
    uint64_t s8;
    uint64_t s9;
    uint64_t s10;
    uint64_t s11;
    uint64_t t3;
    uint64_t t4;
    uint64_t t5;
    uint64_t t6;
};

struct riscv_f_ext_state {
    uint32_t f[32];
    uint32_t fcsr;
};

struct riscv_d_ext_state {
    uint64_t f[32];
    uint32_t fcsr;
};

struct riscv_q_ext_state {
    uint64_t f[64] __attribute__((aligned(16)));
    uint32_t fcsr;
    uint32_t reserved[3];
};

struct riscv_extra_ext_header {
    uint32_t __padding[129] __attribute__((aligned(16)));
    uint32_t reserved;
    struct riscv_ctx_hdr hdr;
};

union riscv_fp_state {
    struct riscv_f_ext_state f;
    struct riscv_d_ext_state d;
    struct riscv_q_ext_state q;
};

struct sigcontext {
    struct riscv_user_regs sc_regs;
    union {
        union riscv_fp_state sc_fpregs;
        struct riscv_extra_ext_header sc_extdesc;
    };
};

typedef struct ucontext {
    unsigned long       uc_flags;
    struct ucontext*    uc_link;
    stack_t             uc_stack;
    __sigset_t          uc_sigmask;
    /* The real kernel ABI reserves room here so glibc's wider (1024-bit) sigset_t can overlay
     * uc_sigmask+this padding without the kernel needing to know about it; we keep the same
     * layout even though our own `__sigset_t` is the narrower 64-signal kernel version, so that
     * `uc_mcontext` lands at the same offset the kernel actually writes to (identical scheme to
     * the aarch64 one). */
    uint8_t             __unused[1024 / 8 - sizeof(__sigset_t)];
    struct sigcontext   uc_mcontext;
} ucontext_t;

/*
 * GPR conversion between the kernel's named-field order and PAL_CONTEXT.regs[32] (ABI x-index).
 * Both directions are field-by-field copies (no memcpy is possible: the kernel does not store
 * GPRs in numeric register order). x0 (regs[0]) is architecturally zero and absent from the
 * frame; it reads as 0 and writes to it are ignored by the kernel on sigreturn.
 */
#define RISCV_GPR_COPY(from, to)      \
    (to)[1]  = (from)->ra;            \
    (to)[2]  = (from)->sp;            \
    (to)[3]  = (from)->gp;            \
    (to)[4]  = (from)->tp;            \
    (to)[5]  = (from)->t0;            \
    (to)[6]  = (from)->t1;            \
    (to)[7]  = (from)->t2;            \
    (to)[8]  = (from)->s0;            \
    (to)[9]  = (from)->s1;            \
    (to)[10] = (from)->a0;            \
    (to)[11] = (from)->a1;            \
    (to)[12] = (from)->a2;            \
    (to)[13] = (from)->a3;            \
    (to)[14] = (from)->a4;            \
    (to)[15] = (from)->a5;            \
    (to)[16] = (from)->a6;            \
    (to)[17] = (from)->a7;            \
    (to)[18] = (from)->s2;            \
    (to)[19] = (from)->s3;            \
    (to)[20] = (from)->s4;            \
    (to)[21] = (from)->s5;            \
    (to)[22] = (from)->s6;            \
    (to)[23] = (from)->s7;            \
    (to)[24] = (from)->s8;            \
    (to)[25] = (from)->s9;            \
    (to)[26] = (from)->s10;           \
    (to)[27] = (from)->s11;           \
    (to)[28] = (from)->t3;            \
    (to)[29] = (from)->t4;            \
    (to)[30] = (from)->t5;            \
    (to)[31] = (from)->t6

static inline void riscv_user_regs_to_array(const struct riscv_user_regs* from, uint64_t* to) {
    to[0] = 0;
    RISCV_GPR_COPY(from, to);
}

static inline void riscv_user_regs_from_array(struct riscv_user_regs* to, const uint64_t* from) {
    to->ra = from[1];
    to->sp = from[2];
    to->gp = from[3];
    to->tp = from[4];
    to->t0 = from[5];
    to->t1 = from[6];
    to->t2 = from[7];
    to->s0 = from[8];
    to->s1 = from[9];
    to->a0 = from[10];
    to->a1 = from[11];
    to->a2 = from[12];
    to->a3 = from[13];
    to->a4 = from[14];
    to->a5 = from[15];
    to->a6 = from[16];
    to->a7 = from[17];
    to->s2 = from[18];
    to->s3 = from[19];
    to->s4 = from[20];
    to->s5 = from[21];
    to->s6 = from[22];
    to->s7 = from[23];
    to->s8 = from[24];
    to->s9 = from[25];
    to->s10 = from[26];
    to->s11 = from[27];
    to->t3 = from[28];
    to->t4 = from[29];
    to->t5 = from[30];
    to->t6 = from[31];
}

/* fpregs is shallow copied by only setting a pointer -- like x86_64 (whose `fpstate` is a
 * pointer), unlike aarch64 where the record has to be located by scanning. The alias is valid
 * for the lifetime of `uc`, and the F/D block's position is fixed (no record-chain scan needed).
 * `fcsr` is the riscv64 conflation of aarch64's FPSR/FPCR: one 32-bit word with FRM (bits 6-8,
 * rounding mode) and FFLAGS (bits 0-4, sticky exception flags). */
static inline void ucontext_to_pal_context(PAL_CONTEXT* context, ucontext_t* uc) {
    riscv_user_regs_to_array(&uc->uc_mcontext.sc_regs, context->regs);
    context->pc = uc->uc_mcontext.sc_regs.pc;
    context->pstate = PAL_PSTATE_EXACT_CONTEXT; /* see pal_arch.h */
    context->fault_address = 0; /* riscv64 sigcontext carries no faulting address: the PAL's
                                   handle_sync_signal() passes siginfo's si_addr to the upcall
                                   instead (see pal_exception.c's `addr` argument) */
    context->esr = 0;
    context->oldmask = 0;

    struct riscv_d_ext_state* fp = &uc->uc_mcontext.sc_fpregs.d;
    context->fpregs = (PAL_XREGS_STATE*)fp;
    context->is_fpregs_used = 1; /* F/D are mandatory for the lp64d ABI we target; a frame taken
                                    while the task had no F/D state saved would be a kernel bug */
    context->fcsr = fp->fcsr;
}

/*
 * Reverse direction: like aarch64 (whose __reserved[] record is inline storage too) this has to
 * physically copy the FP register data when context->fpregs points at an unrelated buffer (e.g.
 * the syscall-entry scratch area in syscallas.S); when it already aliases `uc`'s own frame
 * (ucontext_to_pal_context()'s alias above) the copy is a harmless no-op self-copy. Writing only
 * the fixed 264-byte F/D block never touches an appended vector extension record: the extension
 * header `hdr` lives at offset 520 of the union and the vector payload past the whole sigcontext,
 * both outside this write, so sigreturn still sees a consistent (untouched) V record.
 */
static inline void pal_context_to_ucontext(ucontext_t* uc, PAL_CONTEXT* context) {
    riscv_user_regs_from_array(&uc->uc_mcontext.sc_regs, context->regs);
    uc->uc_mcontext.sc_regs.pc = context->pc;

    struct riscv_d_ext_state* fp = &uc->uc_mcontext.sc_fpregs.d;
    if (context->is_fpregs_used && context->fpregs) {
        memcpy(fp->f, context->fpregs->fpstate.f, sizeof(fp->f));
        fp->fcsr = context->fpregs->fpstate.fcsr;
    } else {
        memset(fp->f, 0, sizeof(fp->f));
        fp->fcsr = 0;
    }
}

/* Handles SIGTRAP for the exact-context-restore protocol (PAL_BRK_RESTORE_CONTEXT, pal_arch.h);
 * defined in pal/src/host/linux/arch/riscv64/pal_arch_exception.c. */
void pal_arch_handle_trap(ucontext_t* uc);
/* Called by the Linux PAL's SIGSYS handler for every seccomp trap (PAL_HAS_SYSCALL_TRAP_HOOK). */
void pal_arch_note_syscall_trap(ucontext_t* uc);

static inline uint64_t ucontext_get_ip(ucontext_t* uc) {
    return uc->uc_mcontext.sc_regs.pc;
}

static inline void ucontext_set_ip(ucontext_t* uc, uint64_t ip) {
    uc->uc_mcontext.sc_regs.pc = ip;
}

/* `si_call_addr` is a macro, we cannot use it as an argument name. */
static inline void ucontext_revert_syscall(ucontext_t* uc, unsigned int arch, int syscall_nr,
                                           void* _si_call_addr) {
    __UNUSED(arch);
    /* man seccomp(2) says that "si_call_addr will show the address of the system call
     * instruction", which is not true - it points to the next instruction past "ecall". The
     * RISC-V `ecall` instruction is always 4 bytes (0x00000073) even when the compressed
     * extension is present, unlike x86_64's variable-length encoding; verified against the
     * running kernel in Phase 0 (Documentation/devel/riscv/plan.md log). */
    uc->uc_mcontext.sc_regs.pc = (uint64_t)_si_call_addr - 4;
    /* A7 carries the syscall number on riscv64 (distinct from A0, the return-value/first-arg
     * register that x86_64's single RAX conflates); the kernel never overwrites it, but we set it
     * explicitly for clarity rather than relying on that. */
    uc->uc_mcontext.sc_regs.a7 = syscall_nr;

    uint8_t* pc = (uint8_t*)uc->uc_mcontext.sc_regs.pc;
    /* `ecall` encodes as the little-endian byte sequence 73 00 00 00. */
    assert(pc[0] == 0x73 && pc[1] == 0x00 && pc[2] == 0x00 && pc[3] == 0x00);
    __UNUSED(pc);
}
