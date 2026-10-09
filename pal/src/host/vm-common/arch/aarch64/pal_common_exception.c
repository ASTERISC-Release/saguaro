/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2024 Intel Corporation */

/*
 * This file contains handling of hardware exceptions (forwarding them to LibOS).
 */

#include "api.h"
#include "pal.h"
#include "pal_common.h"
#include "pal_internal.h"

#include "kernel_interrupts.h"

/* fpregs is shallow copied by only setting a pointer */
static void isr_regs_to_pal_context(PAL_CONTEXT* context, struct isr_regs* regs,
                                    uint64_t faulted_addr) {
    for (size_t i = 0; i < ARRAY_SIZE(regs->regs); i++)
        context->regs[i] = regs->regs[i];

    context->sp    = regs->sp;
    context->pc    = regs->elr;
    context->pstate = regs->spsr;

    context->esr          = 0; /* dummy: the memfault path has no single faulting syndrome */
    context->oldmask      = 0; /* dummy */
    context->fault_address = faulted_addr;

    context->fpregs = regs->fpregs;
    context->is_fpregs_used = 1;

    context->fpsr = 0;
    context->fpcr = 0;
    context->orig_x0 = context->regs[0];
}

/* fpregs is shallow copied by only setting a pointer */
static void pal_context_to_isr_regs(struct isr_regs* regs, PAL_CONTEXT* context) {
    for (size_t i = 0; i < ARRAY_SIZE(regs->regs); i++)
        regs->regs[i] = context->regs[i];

    regs->sp   = context->sp;
    regs->elr  = context->pc;
    regs->spsr = context->pstate;

    regs->fpregs = context->fpregs;
}

int pal_common_perform_memfault_handling(uint64_t faulted_addr, struct isr_regs* regs) {
    pal_event_handler_t upcall = _PalGetExceptionHandler(PAL_EVENT_MEMFAULT);
    if (!upcall)
        return -PAL_ERROR_DENIED;

    struct pal_tcb_vm* curr_tcb = (struct pal_tcb_vm*)pal_get_tcb();
    if (!curr_tcb)
        return -PAL_ERROR_DENIED;

    /* ELR in the isr_regs is the actual user RIP */
    curr_tcb->kernel_thread.context.user_rip = regs->elr;

    PAL_CONTEXT context;
    isr_regs_to_pal_context(&context, regs, faulted_addr);
    (*upcall)(/*is_in_pal=*/false, faulted_addr, &context);
    pal_context_to_isr_regs(regs, &context);

    return 0;
}
