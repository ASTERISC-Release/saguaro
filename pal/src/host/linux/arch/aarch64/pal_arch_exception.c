/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2014 Stony Brook University
 *               2020 Intel Labs
 */

/*
 * The sigaction/sigprocmask logic here has no aarch64-specific content at all -- it only deals
 * with __sigset_t/sigaction struct layout (per-arch headers) and the syscall_rt_sigreturn
 * trampoline (per-arch asm), both supplied by the aarch64 sibling headers/syscall.S, so that
 * part of the C code is identical to the x86_64 version.
 */

#include <stddef.h> /* needed by <linux/signal.h> for size_t */
#include <linux/signal.h>

#include "sigreturn.h"
#include "sigset.h"
#include "syscall.h"
#include "ucontext.h" /* pulls in pal.h -> pal_arch.h, which declares g_pal_tcb_singleton */

/* Milestone A stopgap single-threaded PAL_TCB storage -- see the comment on this declaration in
 * pal/include/arch/aarch64/pal_arch.h for why aarch64 can't just do what %gs does on x86_64. */
PAL_TCB* g_pal_tcb_singleton = NULL;

int arch_do_rt_sigprocmask(int sig, int how) {
    __sigset_t mask;
    __sigemptyset(&mask);
    __sigaddset(&mask, sig);

    return DO_SYSCALL(rt_sigprocmask, how, &mask, NULL, sizeof(__sigset_t));
}

int arch_do_rt_sigaction(int sig, void* handler,
                         const int* async_signals, size_t async_signals_cnt) {
    struct sigaction action = {0};
    action.sa_handler  = handler;
    action.sa_flags    = SA_SIGINFO | SA_ONSTACK | SA_RESTORER;
    action.sa_restorer = syscall_rt_sigreturn;

    /* disallow nested asynchronous signals during exception handling */
    __sigemptyset((__sigset_t*)&action.sa_mask);
    for (size_t i = 0; i < async_signals_cnt; i++)
        __sigaddset((__sigset_t*)&action.sa_mask, async_signals[i]);

    return DO_SYSCALL(rt_sigaction, sig, &action, NULL, sizeof(__sigset_t));
}
