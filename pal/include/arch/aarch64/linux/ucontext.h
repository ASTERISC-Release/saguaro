/* SPDX-License-Identifier: LGPL-3.0-or-later */

#pragma once

#include <stdint.h>

#include "api.h"
#include "assert.h"
#include "pal.h"

/* Structure definitions source: Linux arch/arm64/include/uapi/asm/{sigcontext.h,ucontext.h}.
 * Unlike x86_64's fixed-size FXSAVE-derived `_fpstate`, aarch64's sigcontext stores FP/SIMD (and
 * optionally SVE) state as a chain of variable-length, magic-tagged records inside a 4KB
 * `__reserved` area -- there is no single flat "fpstate pointer" field to alias against
 * PAL_CONTEXT the way x86_64 does, so we scan for the FPSIMD_MAGIC record instead. */

#define UC_FP_XSTATE            1
#define UC_SIGCONTEXT_SS        2
#define UC_STRICT_RESTORE_SS    4

struct _aarch64_ctx {
    uint32_t magic;
    uint32_t size;
};

#define FPSIMD_MAGIC 0x46508001u

struct fpsimd_context {
    struct _aarch64_ctx head; /* magic == FPSIMD_MAGIC, size == sizeof(struct fpsimd_context) */
    uint32_t fpsr;
    uint32_t fpcr;
    __uint128_t vregs[32];
};

struct sigcontext {
    uint64_t fault_address;
    /* AArch64 registers: regs[0..30] == x0..x30 */
    uint64_t regs[31];
    uint64_t sp;
    uint64_t pc;
    uint64_t pstate;
    /* 4K reserved for FP/SIMD state (fpsimd_context) and future expansion (e.g. SVE); terminated
     * by a zeroed `_aarch64_ctx` record (magic == 0). */
    uint8_t __reserved[4096] __attribute__((__aligned__(16)));
};

typedef struct ucontext {
    unsigned long       uc_flags;
    struct ucontext*    uc_link;
    stack_t              uc_stack;
    __sigset_t          uc_sigmask;
    /* The real kernel ABI reserves room here so glibc's wider (1024-bit) sigset_t can overlay
     * uc_sigmask+this padding without the kernel needing to know about it; we keep the same
     * layout even though our own `__sigset_t` is the narrower 64-signal kernel version, so that
     * `uc_mcontext` lands at the same offset the kernel actually writes to. */
    uint8_t             __unused[1024 / 8 - sizeof(__sigset_t)];
    struct sigcontext   uc_mcontext;
} ucontext_t;

static inline struct fpsimd_context* find_fpsimd_context(struct sigcontext* sc) {
    struct _aarch64_ctx* head = (struct _aarch64_ctx*)sc->__reserved;
    while (head->magic != 0) {
        if (head->magic == FPSIMD_MAGIC)
            return (struct fpsimd_context*)head;
        if (head->size == 0)
            break; /* malformed record chain; bail out instead of looping forever */
        head = (struct _aarch64_ctx*)((uint8_t*)head + head->size);
    }
    return NULL;
}

/* fpregs is shallow copied by only setting a pointer */
static inline void ucontext_to_pal_context(PAL_CONTEXT* context, ucontext_t* uc) {
    memcpy(context->regs, uc->uc_mcontext.regs, sizeof(context->regs));
    context->sp = uc->uc_mcontext.sp;
    context->pc = uc->uc_mcontext.pc;
    context->pstate = uc->uc_mcontext.pstate | PAL_PSTATE_EXACT_CONTEXT; /* see pal_arch.h */
    context->fault_address = uc->uc_mcontext.fault_address;
    context->esr = 0; /* not populated: no generic (non-arch-dir) code consumes this today */
    context->oldmask = 0;

    struct fpsimd_context* fp = find_fpsimd_context(&uc->uc_mcontext);
    context->fpregs = (PAL_XREGS_STATE*)fp;
    context->is_fpregs_used = fp ? 1 : 0;
    if (fp) {
        context->fpsr = fp->fpsr;
        context->fpcr = fp->fpcr;
    }
}

/*
 * Unlike x86_64 (whose sigcontext.fpstate is itself a *pointer*, so pal_context_to_ucontext()
 * there can just repoint it at context->fpregs with no data movement at all), aarch64's
 * sigcontext carries FP/SIMD state *inline* inside a fixed 4KB __reserved[] array -- there is no
 * pointer indirection to borrow. So unlike ucontext_to_pal_context() above (which can alias
 * find_fpsimd_context()'s result directly, since that record's lifetime matches `uc`'s), this
 * direction has to physically copy the NEON register data from wherever context->fpregs
 * currently points (which may be a completely unrelated buffer, e.g. the syscall-entry scratch
 * area in syscallas.S) into a freshly-placed fpsimd_context record inside `uc`'s own
 * __reserved[].
 */
static inline void pal_context_to_ucontext(ucontext_t* uc, PAL_CONTEXT* context) {
    memcpy(uc->uc_mcontext.regs, context->regs, sizeof(context->regs));
    uc->uc_mcontext.sp = context->sp;
    uc->uc_mcontext.pc = context->pc;
    uc->uc_mcontext.pstate = context->pstate;
    uc->uc_mcontext.fault_address = context->fault_address;

    struct fpsimd_context* fp = (struct fpsimd_context*)uc->uc_mcontext.__reserved;
    fp->head.magic = FPSIMD_MAGIC;
    fp->head.size = sizeof(*fp);
    if (context->is_fpregs_used && context->fpregs) {
        fp->fpsr = context->fpregs->fpstate.fpsr;
        fp->fpcr = context->fpregs->fpstate.fpcr;
        memcpy(fp->vregs, context->fpregs->fpstate.vregs, sizeof(fp->vregs));
    } else {
        fp->fpsr = 0;
        fp->fpcr = 0;
        memset(fp->vregs, 0, sizeof(fp->vregs));
    }
    /* Terminate the extensible record chain right after this one (a zeroed _aarch64_ctx marks
     * the end, per the kernel ABI -- see find_fpsimd_context()'s loop condition above). */
    struct _aarch64_ctx* terminator = (struct _aarch64_ctx*)((uint8_t*)fp + sizeof(*fp));
    terminator->magic = 0;
    terminator->size = 0;
}

/* Handles SIGTRAP for the exact-context-restore protocol (PAL_BRK_RESTORE_CONTEXT, pal_arch.h);
 * defined in pal/src/host/linux/arch/aarch64/pal_arch_exception.c. */
void pal_arch_handle_trap(ucontext_t* uc);

static inline uint64_t ucontext_get_ip(ucontext_t* uc) {
    return uc->uc_mcontext.pc;
}

static inline void ucontext_set_ip(ucontext_t* uc, uint64_t ip) {
    uc->uc_mcontext.pc = ip;
}

/* `si_call_addr` is a macro, we cannot use it as an argument name. */
static inline void ucontext_revert_syscall(ucontext_t* uc, unsigned int arch, int syscall_nr,
                                           void* _si_call_addr) {
    __UNUSED(arch);
    /* man seccomp(2) says that "si_call_addr will show the address of the system call
     * instruction", which is not true - it points to the next instruction past "svc". Every
     * aarch64 instruction is 4 bytes, unlike x86_64's variable-length encoding. */
    uc->uc_mcontext.pc = (uint64_t)_si_call_addr - 4;
    /* X8 carries the syscall number on aarch64 (distinct from X0, the return-value/first-arg
     * register that x86_64's single RAX conflates); the kernel never overwrites it, but we set
     * it explicitly for clarity rather than relying on that. */
    uc->uc_mcontext.regs[8] = syscall_nr;

    uint8_t* pc = (uint8_t*)uc->uc_mcontext.pc;
    /* `svc #0` encodes as the little-endian byte sequence 01 00 00 d4. */
    assert(pc[0] == 0x01 && pc[1] == 0x00 && pc[2] == 0x00 && pc[3] == 0xd4);
    __UNUSED(pc);
}
