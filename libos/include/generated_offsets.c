#include "generated_offsets_build.h"
#include "libos_internal.h"
#include "libos_tcb.h"

const char* generated_offsets_name = "LIBOS";

const struct generated_offset generated_offsets[] = {
    OFFSET_T(LIBOS_TCB_OFF, PAL_TCB, libos_tcb),
    OFFSET_T(LIBOS_TCB_LIBOS_STACK_OFF, libos_tcb_t, libos_stack_bottom),
    OFFSET_T(LIBOS_TCB_SCRATCH_PC_OFF, libos_tcb_t, syscall_scratch_pc),

    OFFSET_T(PAL_CONTEXT_FPREGS_OFF, struct PAL_CONTEXT, fpregs),
    OFFSET_T(PAL_CONTEXT_FPREGS_USED_OFF, struct PAL_CONTEXT, is_fpregs_used),
#if defined(__x86_64__)
    OFFSET_T(PAL_CONTEXT_MXCSR_OFF, struct PAL_CONTEXT, mxcsr),
    OFFSET_T(PAL_CONTEXT_FPCW_OFF, struct PAL_CONTEXT, fpcw),
#elif defined(__aarch64__)
    /* aarch64's PAL_CONTEXT has no named x86-style GPR fields (see pal_arch.h): `regs` is a flat
     * x0..x30 array matching the real kernel ABI, so syscallas.S indexes into it directly off
     * one base offset rather than needing one named offset per register. */
    OFFSET_T(PAL_CONTEXT_REGS_OFF, struct PAL_CONTEXT, regs),
    OFFSET_T(PAL_CONTEXT_SP_OFF, struct PAL_CONTEXT, sp),
    OFFSET_T(PAL_CONTEXT_PC_OFF, struct PAL_CONTEXT, pc),
    OFFSET_T(PAL_CONTEXT_PSTATE_OFF, struct PAL_CONTEXT, pstate),
    OFFSET_T(PAL_CONTEXT_FAULT_ADDRESS_OFF, struct PAL_CONTEXT, fault_address),
    OFFSET_T(PAL_CONTEXT_ESR_OFF, struct PAL_CONTEXT, esr),
    OFFSET_T(PAL_CONTEXT_OLDMASK_OFF, struct PAL_CONTEXT, oldmask),
    OFFSET_T(PAL_CONTEXT_FPSR_OFF, struct PAL_CONTEXT, fpsr),
    OFFSET_T(PAL_CONTEXT_FPCR_OFF, struct PAL_CONTEXT, fpcr),
    /* syscallas.S allocates the whole struct with one `sub sp, sp, #PAL_CONTEXT_SIZE` and fills
     * it in via offset-based stores, rather than x86_64's sequence of individual pushes -- so,
     * unlike x86_64's syscallas.S, it needs the total size as a constant. */
    DEFINE(PAL_CONTEXT_SIZE, sizeof(struct PAL_CONTEXT)),
    /* immediate of the `brk` that asks the PAL to restore a context exactly (pal_arch.h) */
    DEFINE(PAL_BRK_RESTORE_CONTEXT, PAL_BRK_RESTORE_CONTEXT),
#endif

    DEFINE(LIBOS_XSTATE_ALIGN, LIBOS_XSTATE_ALIGN),
    DEFINE(RED_ZONE_SIZE, RED_ZONE_SIZE),

    OFFSET_END,
};
