/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2023 Intel Corporation */

/*
 * aarch64 FP/SIMD state management: the counterpart of vm-common/kernel_xsave.c.
 *
 * x86_64 saves the FP state with the `xsave`/`xrstor` instructions into an XSAVE area whose
 * content is dictated by CPUID/XCR0. aarch64 has no equivalent single instruction for the
 * architectural FP state (there is no "save all FP registers" op at all -- the kernel's own
 * fpsimd.c saves V0-V31 + FPCR/FPSR with ordinary ldp/stp), so `do_xsave`/`do_xrstor` are plain
 * C functions doing exactly that into a PAL_XREGS_STATE area (which on aarch64 is defined by
 * the kernel's sigcontext ABI, not by us: see pal/include/arch/aarch64/pal_arch.h).
 *
 * FP access must be enabled at EL1 *and* EL0 (CPACR_EL1.FPEN) before any FP instruction can
 * execute; that is the role of xsave_init(). (QEMU virt and Cortex-A cores trap EL0 FP to EL1
 * when FPEN=0; Gramine's vm kernel would then have to emulate, so we simply never disable it.)
 */

#include <stdint.h>

#include "pal.h" /* PAL_XREGS_STATE / PAL_FPSTATE (via arch/aarch64/pal_arch.h) */

#include "kernel_xsave.h"

/* fpsimd_context magic from the UAPI fpsigcontext record; the kernel-internal include layer
 * deliberately doesn't pull linux/ucontext.h (a Linux-PAL header), so keep the constant here
 * with the same value Linux uses. */
#ifndef FPSIMD_MAGIC
#define FPSIMD_MAGIC 0x46508001u
#endif

uint32_t g_xsave_size = sizeof(PAL_XREGS_STATE);

const uint64_t g_xsave_reset_state[VM_XSAVE_RESET_STATE_SIZE / sizeof(uint64_t)] = { 0 };

static inline uint32_t read_fpsr(void) {
    uint32_t v;
    __asm__ volatile("mrs %0, fpsr" : "=r"(v));
    return v;
}

static inline uint32_t read_fpcr(void) {
    uint32_t v;
    __asm__ volatile("mrs %0, fpcr" : "=r"(v));
    return v;
}

static inline void write_fpsr(uint32_t v) {
    __asm__ volatile("msr fpsr, %0" :: "r"(v));
}

static inline void write_fpcr(uint32_t v) {
    __asm__ volatile("msr fpcr, %0" :: "r"(v));
}

int xsave_init(void) {
    uint64_t cpacr;
    __asm__ volatile("mrs %0, cpacr_el1" : "=r"(cpacr));
    cpacr |= 0x3UL << 20; /* CPACR_EL1.FPEN = 0b11: FP accessible at EL1 and EL0 */
    __asm__ volatile("msr cpacr_el1, %0" :: "r"(cpacr));
    __asm__ volatile("isb" ::: "memory");

    /* start from a defined FP environment (round-to-nearest, exceptions masked) */
    write_fpcr(0);
    write_fpsr(0);
    return 0;
}

/* void do_xsave(PAL_XREGS_STATE* fpsimd_area) -- save V0-V31 + FPCR/FPSR.
 * Also stamps the fpsimd_context magic/size so any consumer that inspects the area (the same
 * bytes are visible to LibOS via PAL_CONTEXT.fpregs) sees a well-formed record. */
void do_xsave(void* fpsimd_area) {
    PAL_XREGS_STATE* area = fpsimd_area;

    area->fpstate.magic = FPSIMD_MAGIC;
    area->fpstate.size = sizeof(PAL_FPSTATE);
    area->fpstate.fpsr = read_fpsr();
    area->fpstate.fpcr = read_fpcr();

    uint64_t addr = (uint64_t)&area->fpstate.vregs[0];
    __asm__ volatile(
        "stp q0,  q1,  [%[p], #0]   \n"
        "stp q2,  q3,  [%[p], #32]  \n"
        "stp q4,  q5,  [%[p], #64]  \n"
        "stp q6,  q7,  [%[p], #96]  \n"
        "stp q8,  q9,  [%[p], #128] \n"
        "stp q10, q11, [%[p], #160] \n"
        "stp q12, q13, [%[p], #192] \n"
        "stp q14, q15, [%[p], #224] \n"
        "stp q16, q17, [%[p], #256] \n"
        "stp q18, q19, [%[p], #288] \n"
        "stp q20, q21, [%[p], #320] \n"
        "stp q22, q23, [%[p], #352] \n"
        "stp q24, q25, [%[p], #384] \n"
        "stp q26, q27, [%[p], #416] \n"
        "stp q28, q29, [%[p], #448] \n"
        "stp q30, q31, [%[p], #480] \n"
        : [p] "+r"(addr)
        :
        : "memory");
}

/* void do_xrstor(const PAL_XREGS_STATE* fpsimd_area) -- restore V0-V31 + FPCR/FPSR. */
void do_xrstor(void* fpsimd_area) {
    PAL_XREGS_STATE* area = fpsimd_area;

    write_fpsr(area->fpstate.fpsr);
    write_fpcr(area->fpstate.fpcr);

    uint64_t addr = (uint64_t)&area->fpstate.vregs[0];
    __asm__ volatile(
        "ldp q0,  q1,  [%[p], #0]   \n"
        "ldp q2,  q3,  [%[p], #32]  \n"
        "ldp q4,  q5,  [%[p], #64]  \n"
        "ldp q6,  q7,  [%[p], #96]  \n"
        "ldp q8,  q9,  [%[p], #128] \n"
        "ldp q10, q11, [%[p], #160] \n"
        "ldp q12, q13, [%[p], #192] \n"
        "ldp q14, q15, [%[p], #224] \n"
        "ldp q16, q17, [%[p], #256] \n"
        "ldp q18, q19, [%[p], #288] \n"
        "ldp q20, q21, [%[p], #320] \n"
        "ldp q22, q23, [%[p], #352] \n"
        "ldp q24, q25, [%[p], #384] \n"
        "ldp q26, q27, [%[p], #416] \n"
        "ldp q28, q29, [%[p], #448] \n"
        "ldp q30, q31, [%[p], #480] \n"
        : [p] "+r"(addr)
        :
        : "memory");
}
