/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2020 Intel Corporation
 *                    Borys Popławski <borysp@invisiblethingslab.com>
 */

#include "asan.h"
#include "libos_defs.h"
#include "libos_internal.h"
#include "libos_lock.h"
#include "libos_signal.h"
#include "libos_table.h"
#include "libos_tcb.h"
#include "libos_thread.h"
#include "linux_abi/errors.h"
#include "pal.h"

#if defined(__x86_64__)
#define SUD_AUDIT_ARCH 0xc000003eU /* AUDIT_ARCH_X86_64 */
#elif defined(__aarch64__)
#define SUD_AUDIT_ARCH 0xc00000b7U /* AUDIT_ARCH_AARCH64 */
#elif defined(__riscv)
#define SUD_AUDIT_ARCH 0xc00000f3U /* AUDIT_ARCH_RISCV64 */
#else
#define SUD_AUDIT_ARCH 0
#endif

typedef arch_syscall_arg_t (*six_args_syscall_t)(arch_syscall_arg_t, arch_syscall_arg_t,
                                                 arch_syscall_arg_t, arch_syscall_arg_t,
                                                 arch_syscall_arg_t, arch_syscall_arg_t);

/* The address after the application's `syscall` instruction (what a signal frame shows as RIP):
 * on the VM and TDX PALs the system call enters the LibOS through the kernel's stub, whose address
 * is the context's RIP, and the user RIP sits in the PAL TCB (as prepare_sigframe() reads it). */
static uintptr_t syscall_user_ip(PAL_CONTEXT* context) {
    if (!strcmp(g_pal_public_state->host_type, "VM") ||
            !strcmp(g_pal_public_state->host_type, "TDX")) {
        uintptr_t user_rip;
        memcpy(&user_rip, (char*)pal_get_tcb() + g_pal_public_state->vm_user_rip_offset,
               sizeof(user_rip));
        return user_rip;
    }
    return pal_context_get_ip(context);
}

/* prctl(PR_SET_SYSCALL_USER_DISPATCH): whether this system call is to become SIGSYS instead of
 * running (Linux's syscall_user_dispatch()): made from outside the allowed range, with the
 * selector byte not saying SYSCALL_DISPATCH_FILTER_ALLOW. Returns true when a signal was forced;
 * the caller leaves the syscall number in the return register and delivers the signal. */
static bool syscall_user_dispatch(struct libos_thread* cur, PAL_CONTEXT* context,
                                  unsigned long sysnr) {
    uintptr_t ip = syscall_user_ip(context);
    if (ip - cur->syscall_user_dispatch.start < cur->syscall_user_dispatch.len)
        return false;
    char state = 1; /* SYSCALL_DISPATCH_FILTER_BLOCK: no selector means every call is caught */
    if (cur->syscall_user_dispatch.selector) {
        if (!is_user_memory_readable(cur->syscall_user_dispatch.selector, 1)) {
            /* Linux kills the thread with SIGSEGV; the fault is the selector's */
            siginfo_t info = { .si_signo = SIGSEGV, .si_code = SEGV_MAPERR,
                               .si_addr = cur->syscall_user_dispatch.selector };
            libos_force_signal(&info);
            return true;
        }
        state = __atomic_load_n(cur->syscall_user_dispatch.selector, __ATOMIC_RELAXED);
        if (state == 0) /* SYSCALL_DISPATCH_FILTER_ALLOW */
            return false;
    }
    siginfo_t info = { .si_signo = SIGSYS, .si_errno = 0, .si_code = 2 /* SYS_USER_DISPATCH */ };
    info.si_call_addr = (void*)ip;
    info.si_syscall   = (int)sysnr;
    info.si_arch      = SUD_AUDIT_ARCH;
    if (state != 1) {
        /* an unknown selector value: Linux kills the thread with SIGSYS (SI_KERNEL) */
        info.si_code = SI_KERNEL;
    }
    libos_force_signal(&info);
    return true;
}

/*
 * `context` is expected to be placed at the bottom of Gramine-internal stack.
 * If you change this function please also look at `libos_syscall_rt_sigsuspend`!
 */
noreturn void libos_emulate_syscall(PAL_CONTEXT* context) {
    LIBOS_TCB_SET(context.regs, context);

    unsigned long sysnr = pal_context_get_syscall(context);
    arch_syscall_arg_t ret = 0;

    struct libos_thread* cur_thread = get_cur_thread();
    if (__builtin_expect(__atomic_load_n(&cur_thread->syscall_user_dispatch.on, __ATOMIC_ACQUIRE), 0) &&
            sysnr != GRAMINE_CUSTOM_SYSCALL_NR && syscall_user_dispatch(cur_thread, context, sysnr)) {
        /* the call does not run; its number stays in the return register, as Linux leaves it */
        LIBOS_TCB_SET(context.syscall_nr, sysnr);
        ret = (arch_syscall_arg_t)sysnr;
        goto out;
    }

    if (sysnr == GRAMINE_CUSTOM_SYSCALL_NR) {
        unsigned long args[] = { ALL_SYSCALL_ARGS(context) };
        ret = handle_libos_call(args[0], args[1], args[2]);
    } else {
        if (sysnr >= LIBOS_SYSCALL_BOUND || !libos_syscall_table[sysnr]) {
            warn_unsupported_syscall(sysnr);
            ret = -ENOSYS;
            goto out;
        }

        LIBOS_TCB_SET(context.syscall_nr, sysnr);
        six_args_syscall_t syscall_func = (six_args_syscall_t)libos_syscall_table[sysnr];

        debug_print_syscall_before(sysnr, ALL_SYSCALL_ARGS(context));
        ret = syscall_func(ALL_SYSCALL_ARGS(context));
        debug_print_syscall_after(sysnr, ret, ALL_SYSCALL_ARGS(context));
    }
out:
    /* Architectures whose return register aliases the first argument register save that
     * argument here, so that restart_syscall() can restore it (see libos_internal_arch.h). */
    libos_arch_save_restart_arg0(context);
    pal_context_set_retval(context, ret);

    /* Some syscalls e.g. `sigreturn` could have changed context and in reality we might be not
     * returning from a syscall. */
    if (!handle_signal(context) && LIBOS_TCB_GET(context.syscall_nr) >= 0) {
        switch (ret) {
            case -ERESTARTNOHAND:
            case -ERESTARTSYS:
            case -ERESTARTNOINTR:
                restart_syscall(context, sysnr);
                break;
            default:
                break;
        }
    }

    struct libos_thread* current = get_cur_thread();
    if (current->has_saved_sigmask) {
        lock(&current->lock);
        set_sig_mask(current, &current->saved_sigmask);
        unlock(&current->lock);
        current->has_saved_sigmask = false;
    }

    LIBOS_TCB_SET(context.syscall_nr, -1);
    LIBOS_TCB_SET(context.regs, NULL);

    return_from_syscall(context);
}

__attribute_no_sanitize_address
noreturn void return_from_syscall(PAL_CONTEXT* context) {
#ifdef ASAN
    uintptr_t libos_stack_bottom = (uintptr_t)LIBOS_TCB_GET(libos_stack_bottom);
    asan_unpoison_region(libos_stack_bottom - LIBOS_THREAD_LIBOS_STACK_SIZE,
                         LIBOS_THREAD_LIBOS_STACK_SIZE);
#endif
    _return_from_syscall(context);
}
