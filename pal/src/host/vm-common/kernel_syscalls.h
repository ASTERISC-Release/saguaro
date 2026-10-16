/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2023 Intel Corporation */

/* Declarations for syscalls. */

#pragma once

/* we only use MSRs relevant to 64-bit SYSCALL/SYSRET flows; thus we don't use legacy MSR_CSTAR */
/* these MSRs are allowed to be natively accessed in Intel TDX, so they don't need hardening */
#define MSR_STAR         0xc0000081 /* target CS/SS selectors from GDT */

struct per_cpu_data;
/* LSTAR of this CPU: syscall_asm_gs for a thread that owns %gs, syscall_asm otherwise */
void syscalls_select_entry(struct per_cpu_data* cpu, bool gs);
#define MSR_LSTAR        0xc0000082 /* target 64-bit RIP on `syscall` instruction */
#define MSR_SYSCALL_MASK 0xc0000084 /* RFLAGS mask to apply on `syscall` instruction */

/* rt_sigreturn has a special return-from-syscall path, see kernel_events.S */
#define SYSCALL_RT_SIGRETURN 15

int syscalls_init(void);

struct isr_regs;
/* #GP handler: an `int $0x80` of user mode (error code: IDT vector 0x80) becomes a system-call
 * entry into the LibOS; returns false when the #GP is something else */
bool syscalls_emulate_int80(struct isr_regs* regs);
