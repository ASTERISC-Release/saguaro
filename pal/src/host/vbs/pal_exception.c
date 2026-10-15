/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 Gramine contributors */

/*
 * Exceptions of the vbs PAL proper. A fault in packed code reaches W's exception handler (named by
 * the static unwind data of the packed code, see scripts/vbs/pack.py), which calls
 * pal_vbs_exception() here with the registers. As the Linux PAL does in its signal handler, this
 * builds a PAL_CONTEXT and calls the LibOS's handler for the event (which may redirect the context
 * to the application's signal handler); the registers then go back to W, which resumes the thread.
 *
 * Two special breakpoints: `brk #PAL_BRK_RESTORE_CONTEXT` (the LibOS's exact-context restore,
 * pal_arch.h: x0 = the PAL_CONTEXT to resume) and the svc stub's `brk #0x5653` (a packed object's
 * syscall slot was never filled: a bug, reported).
 */

#include "api.h"
#include "pal.h"
#include "pal_error.h"
#include "pal_internal.h"
#include "pal_vbs.h"

#define BRK(imm) (0xd4200000u | ((uint32_t)(imm) << 5))
#define BRK_UNFILLED_SLOT 0x5653

extern char __text_start[] __attribute__((visibility("hidden")));
extern char __text_end[] __attribute__((visibility("hidden")));

static void fault_to_context(const struct pal_vbs_fault* f, PAL_CONTEXT* ctx, PAL_XREGS_STATE* xs) {
    memset(ctx, 0, sizeof(*ctx));
    memcpy(ctx->regs, f->regs, sizeof(ctx->regs));
    ctx->sp = f->sp;
    ctx->pc = f->pc;
    ctx->pstate = f->pstate | PAL_PSTATE_EXACT_CONTEXT;
    ctx->fault_address = f->addr;
    memset(xs, 0, sizeof(*xs));
    xs->fpstate.magic = 0x46508001; /* FPSIMD_MAGIC */
    xs->fpstate.size = sizeof(xs->fpstate);
    xs->fpstate.fpsr = f->fpsr;
    xs->fpstate.fpcr = f->fpcr;
    memcpy(xs->fpstate.vregs, f->vregs, sizeof(xs->fpstate.vregs));
    ctx->fpregs = xs;
    ctx->fpsr = f->fpsr;
    ctx->fpcr = f->fpcr;
    ctx->is_fpregs_used = 1;
}

static void context_to_fault(const PAL_CONTEXT* ctx, struct pal_vbs_fault* f) {
    memcpy(f->regs, ctx->regs, sizeof(f->regs));
    f->sp = ctx->sp;
    f->pc = ctx->pc;
    f->pstate = ctx->pstate & ~PAL_PSTATE_EXACT_CONTEXT;
    if (ctx->is_fpregs_used && ctx->fpregs) {
        f->fpsr = ctx->fpregs->fpstate.fpsr;
        f->fpcr = ctx->fpregs->fpstate.fpcr;
        memcpy(f->vregs, ctx->fpregs->fpstate.vregs, sizeof(f->vregs));
    }
}

/* called by W (pal_vbs_boot.c) on the faulting thread; 0 = resume with the (changed) registers */
int64_t pal_vbs_exception(struct pal_vbs_fault* f);
int64_t pal_vbs_exception(struct pal_vbs_fault* f) {
    if (f->kind == PAL_VBS_FAULT_ILLEGAL) {
        uint32_t insn = *(const uint32_t*)(uintptr_t)f->pc; /* packed code is readable */
        if (insn == BRK(PAL_BRK_RESTORE_CONTEXT)) {
            PAL_CONTEXT* saved = (PAL_CONTEXT*)(uintptr_t)f->regs[0];
            saved->pstate &= ~PAL_PSTATE_EXACT_CONTEXT;
            context_to_fault(saved, f);
            return 0;
        }
        if (insn == BRK(BRK_UNFILLED_SLOT))
            log_error("vbs: a packed object made a system call before its syscall slot was filled "
                      "(pc %#lx)", f->pc);
    }

    enum pal_event event;
    switch (f->kind) {
        case PAL_VBS_FAULT_MEMORY:     event = PAL_EVENT_MEMFAULT; break;
        case PAL_VBS_FAULT_ILLEGAL:    event = PAL_EVENT_ILLEGAL; break;
        case PAL_VBS_FAULT_ARITHMETIC: event = PAL_EVENT_ARITHMETIC_ERROR; break;
        default: return -1;
    }

    uint64_t tpidr; /* the thread pointer (rewritten by pack.py to the TEB) */
    __asm__ volatile("mrs %0, tpidr_el0" : "=r"(tpidr));
    log_debug("vbs: %s at pc %#lx addr %#lx: x16 %#lx lr %#lx sp %#lx x0 %#lx tpidr %#lx",
              pal_event_name(event), f->pc, f->addr, f->regs[16], f->regs[30], f->sp, f->regs[0],
              tpidr);
    bool is_in_pal = f->pc >= (uintptr_t)__text_start && f->pc < (uintptr_t)__text_end;
    pal_event_handler_t upcall = _PalGetExceptionHandler(event);
    if (!upcall || is_in_pal) {
        log_error("vbs: unhandled %s (Windows exception %#lx) at pc %#lx, address %#lx%s",
                  pal_event_name(event), f->code, f->pc, f->addr, is_in_pal ? " in the PAL" : "");
        return -1;
    }

    PAL_CONTEXT ctx;
    PAL_XREGS_STATE xs __attribute__((aligned(PAL_XSTATE_ALIGN)));
    fault_to_context(f, &ctx, &xs);
    upcall(is_in_pal, f->addr, &ctx);
    context_to_fault(&ctx, f);
    return 0;
}
