/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/*
 * riscv64 syscall-site patching and SIGILL classification (port of the aarch64
 * libos_syscall_patch.c).
 *
 * On riscv64 this file deliberately contains *no* rewriting machinery: the port's design
 * decision (Documentation/devel/riscv/plan.md, "Interception mechanism") is seccomp-trap only,
 * with no syscall-site rewriting. Beyond policy there is a hard technical asymmetry vs the
 * aarch64 prototype: aarch64 replaces a 4-byte `svc` with a 4-byte `b` to a stub *only because
 * the branch displacement fits*; a riscv64 `jal` reaches +-1MiB and a full 64-bit absolute jump
 * needs an auipc+ld pair (8 bytes) or a gp/tp-anchored indirection -- both bigger or
 * state-dependent, i.e. the riscv64 version of the prototype would be strictly more fragile than
 * the aarch64 one, for a fast path (the aarch64 measurements showed ~15% on syscall-heavy
 * workloads) that the seccomp FILTER already makes cheaper here by skipping the LibOS upcall
 * round-trip only when it pays to. `sys.rewrite_syscalls = true` is therefore not supported on
 * riscv64; if it is ever implemented, the hooks below are the insertion points.
 */

#include <stdbool.h>
#include <stdint.h>

#include "pal.h"

bool libos_try_patch_syscall_site(uintptr_t pc) {
    __UNUSED(pc);
    return false; /* unsupported on riscv64, see the file comment */
}

bool libos_syscall_site_was_patched(uintptr_t pc) {
    __UNUSED(pc);
    return false;
}

/*
 * SIGILL classification for Gramine-internal addresses (illegal_upcall()). Whitelists exactly
 * the `ecall` inside the LibOS's own rt_sigreturn trampoline that prepare_sigframe() installs as
 * every guest handler's return address -- the same scheme as aarch64 finding #5, since riscv64
 * (like arm64) has no application-visible SA_RESTORER at all. The trap's PC is already reverted
 * to the trapping instruction (ucontext_revert_syscall() in the shared SIGSYS handler), so
 * whitelist precisely: anywhere in the trampoline's few words whose content is literally `ecall`
 * (0x00000073), discovered by inspecting the instruction rather than hardcoding an offset.
 * Anything else in Gramine-internal memory taking an illegal instruction remains a Gramine bug.
 */
bool libos_arch_is_intentional_internal_insn(uintptr_t addr) {
    void* tramp = libos_rt_sigreturn_trampoline_addr();
    return addr >= (uintptr_t)tramp && addr < (uintptr_t)tramp + 16
           && *(const uint32_t*)addr == 0x00000073 /* ecall */;
}

/* riscv64 has no analog of aarch64's stale pointer-authentication signatures (no PAC in the
 * ISA); no repair is ever possible. */
bool libos_arch_repair_illegal_insn(PAL_CONTEXT* context) {
    __UNUSED(context);
    return false;
}
