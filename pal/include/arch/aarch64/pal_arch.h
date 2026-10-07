/* SPDX-License-Identifier: LGPL-3.0-or-later */

/*
 * This file contains definition of aarch64-specific aspects of PAL.
 */

#ifndef INSIDE_PAL_H
// TODO: fix this
#error This header is usable only inside pal.h (due to a cyclic dependency).
#endif

#pragma once

#include <assert.h>
#include <stdint.h>

#include "api.h"
#include "cpu.h"
#include "pal.h"

typedef struct pal_tcb PAL_TCB;

#define PAL_LIBOS_TCB_SIZE 256


#define STACK_PROTECTOR_CANARY_DEFAULT  0xbadbadbadbadUL

typedef struct pal_tcb {
    struct pal_tcb* self;
    /* random per-thread canary used by GCC's stack protector; must be at tpidr_el0+0x8 because
     * we specify `-mstack-protector-guard=sysreg -mstack-protector-guard-reg=tpidr_el0
     * -mstack-protector-guard-offset=8` (see top-level meson.build) -- this is the aarch64
     * analog of x86_64's %gs:[0x8] scheme. */
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

#include "pal_host_arch.h"

static inline PAL_TCB* pal_get_tcb(void) {
    PAL_TCB* tcb;
    __asm__("mrs %0, tpidr_el0" : "=r"(tcb));
    return tcb;
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
};

/* aarch64 has no segment registers; nothing stands in for x86_64's `union pal_csgsfs`. */

/*
 * aarch64 FP/SIMD context, i.e. the analog of x86_64's FXSAVE/XSAVE-derived PAL_FPSTATE.
 * Mirrors the kernel's `struct fpsimd_context` (32 128-bit V registers + FPSR/FPCR); unlike
 * x86_64's XSAVE area there is no magic-number/feature-bitmap validation scheme here, and (for
 * now, scope of the aarch64 port) no SVE/SVE2 extended-register state.
 */
typedef struct {
    __uint128_t vregs[32];
    uint32_t fpsr;
    uint32_t fpcr;
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
 * this architecture, not a Gramine-specific choice.
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
};
typedef struct PAL_CONTEXT PAL_CONTEXT;

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
