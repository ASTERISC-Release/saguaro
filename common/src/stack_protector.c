/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2021 Intel Corporation
 *                    Borys Popławski <borysp@invisiblethingslab.com>
 */

#include <stdint.h>
#include <stdnoreturn.h>

#include "callbacks.h"
#include "log.h"

#if !defined(__x86_64__)
/*
 * x86_64 uses a register-relative stack-protector-guard scheme (-mstack-protector-guard=tls
 * -mstack-protector-guard-reg=%gs -mstack-protector-guard-offset=8, set in the top-level
 * meson.build), reading the canary straight out of PAL_TCB -- a real per-thread random value
 * (see pal_tcb_arch_set_stack_canary()), and never referencing this symbol at all. Architectures
 * without such a scheme (aarch64 and riscv64, for now: see pal_arch.h's g_pal_tcb_singleton
 * comment for why they don't have one yet either) fall back to GCC's generic stack protector,
 * which expects a single global `__stack_chk_guard` instead.
 *
 * TODO: this is a fixed, compile-time value, not randomized per-boot or per-thread the way
 * x86_64's scheme is -- weaker, but not a regression for the aarch64/riscv64 ports, which have
 * no per-thread canary mechanism to begin with. Randomize via PalRandomBitsRead() once PAL init
 * order makes that straightforward to wire up before the first stack-protected function runs.
 */
uintptr_t __stack_chk_guard = 0xbadbadbadbadUL;
#endif

/* declare here to silence GCC's "error: no previous prototype" */
noreturn void __stack_chk_fail(void);

noreturn void __stack_chk_fail(void) {
    log_always("Stack protector: Saguaro internal stack corruption detected");
    abort();
}
