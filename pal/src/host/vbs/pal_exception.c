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

/* `mrs Xt, ctr_el0`: the cache type register is not readable at EL0 in a VBS enclave (it traps as
 * an illegal instruction), but glibc reads it, e.g. sysconf(_SC_LEVEL1_DCACHE_LINESIZE), which
 * NGINX calls at start. It is emulated with a fixed value: 64-byte minimum instruction and data
 * cache lines, PIPT instruction cache, 64-byte (16-word) exclusives and write-back granules, and
 * neither IDC nor DIC (so software that maintains caches does all of it). */
#define MRS_CTR_EL0_MASK 0xffffffe0u
#define MRS_CTR_EL0      0xd53b0020u
#define VBS_CTR_EL0      0x8444c004ul

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

/* Everything the trampoline needs, placed on the thread's alternate stack. */
struct vbs_fault_frame {
    struct pal_vbs_fault f;
    PAL_CONTEXT ctx;
    PAL_XREGS_STATE xs __attribute__((aligned(PAL_XSTATE_ALIGN)));
};

/* resumes `ctx` exactly: brk #PAL_BRK_RESTORE_CONTEXT with x0 = ctx, served by W's handler and
 * pal_vbs_exception() below (the protocol the LibOS uses too) */
static noreturn void restore_context(PAL_CONTEXT* ctx) {
    register PAL_CONTEXT* x0 __asm__("x0") = ctx;
    __asm__ volatile("brk %1" : : "r"(x0), "i"(PAL_BRK_RESTORE_CONTEXT) : "memory");
    __builtin_unreachable();
}

/*
 * Second step of a fault: runs on the thread's alternate stack in normal execution (no exception
 * dispatch in progress), calls the LibOS's handler, which may write a signal frame on the
 * application's stack, then resumes the resulting context.
 */
noreturn void pal_vbs_fault_trampoline(struct vbs_fault_frame* fr);
noreturn void pal_vbs_fault_trampoline(struct vbs_fault_frame* fr) {
    enum pal_event event = fr->f.kind == PAL_VBS_FAULT_MEMORY ? PAL_EVENT_MEMFAULT
                           : fr->f.kind == PAL_VBS_FAULT_ILLEGAL ? PAL_EVENT_ILLEGAL
                           : PAL_EVENT_ARITHMETIC_ERROR;
    fault_to_context(&fr->f, &fr->ctx, &fr->xs);
    pal_event_handler_t upcall = _PalGetExceptionHandler(event);
    upcall(/*is_in_pal=*/false, fr->f.addr, &fr->ctx);
    fr->ctx.pstate &= ~PAL_PSTATE_EXACT_CONTEXT;
    restore_context(&fr->ctx);
}

/*
 * Called by W (pal_vbs_boot.c) from its exception handler, on the faulting stack about 3 KiB below
 * the faulting SP, with the registers in `f`; returns 0 to resume with the (changed) registers in
 * `f`, negative if the fault is not handled (the process ends).
 *
 * The LibOS cannot be called from here: delivering a signal writes the application's signal frame
 * just below the faulting SP -- where the dispatcher's records and this very frame are. So the
 * fault is copied to the thread's alternate stack and the thread is redirected to
 * pal_vbs_fault_trampoline() there; the trampoline resumes the final context with the
 * restore-context breakpoint, which comes back here.
 */
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
        if ((insn & MRS_CTR_EL0_MASK) == MRS_CTR_EL0) {
            uint32_t rt = insn & 0x1f;
            if (rt < ARRAY_SIZE(f->regs))
                f->regs[rt] = VBS_CTR_EL0; /* (rt 31 is xzr: nothing to write) */
            f->pc += 4;
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

    log_debug("vbs: %s at pc %#lx addr %#lx: x16 %#lx lr %#lx sp %#lx x0 %#lx", pal_event_name(event),
              f->pc, f->addr, f->regs[16], f->regs[30], f->sp, f->regs[0]);

    bool is_in_pal = f->pc >= (uintptr_t)__text_start && f->pc < (uintptr_t)__text_end;
    PAL_VBS_TCB* tcb = (PAL_VBS_TCB*)pal_get_tcb();
    if (!_PalGetExceptionHandler(event) || is_in_pal || !tcb || !tcb->alt_stack) {
        log_error("vbs: unhandled %s (Windows exception %#lx) at pc %#lx, address %#lx%s",
                  pal_event_name(event), f->code, f->pc, f->addr, is_in_pal ? " in the PAL" : "");
        return -1;
    }

    /* place the fault on the alternate stack (below the current use if the thread faulted while
     * already there, e.g. in a signal handler of the LibOS) */
    uintptr_t lo = (uintptr_t)tcb->alt_stack, hi = lo + VBS_ALT_STACK_SIZE;
    uintptr_t top = (f->sp > lo && f->sp <= hi) ? f->sp - 4096 : hi;
    if (top < lo + sizeof(struct vbs_fault_frame) + 16 * 1024) {
        log_error("vbs: nested faults exhausted the alternate stack");
        return -1;
    }
    struct vbs_fault_frame* fr =
        (struct vbs_fault_frame*)ALIGN_DOWN(top - sizeof(*fr), PAL_XSTATE_ALIGN);
    memcpy(&fr->f, f, sizeof(*f));
    f->regs[0] = (uintptr_t)fr;
    f->sp = ALIGN_DOWN((uintptr_t)fr - 64, 16);
    f->pc = (uintptr_t)&pal_vbs_fault_trampoline;
    return 0;
}
