/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/*
 * libos_elf_entry-riscv64.S
 *
 * Jump into the loaded application's entry point with a clean register state, mirroring what
 * the riscv64 kernel itself guarantees at execve(): every GPR zeroed except sp (the new initial
 * stack), fcsr cleared, and a 16-byte-aligned sp.
 *
 * RISC-V-specific notes vs the aarch64 sibling:
 * - `a0` must be zero: it is the one register the entry contract gives a meaning to. glibc's
 *   _start passes it to __libc_start_main() as `rtld_fini`, which registers a non-zero value as
 *   an exit handler. A dynamic executable is entered through ld.so, which sets it itself, but a
 *   static one sees our value: with the entry address left in a0, exit() called _start again.
 *   The jump target therefore travels in t1, a temporary no startup code reads (riscv64 has no
 *   register-free indirect jump, so one register cannot be zero).
 * - `gp` is zeroed exactly like the kernel's trapframe does at exec. Applications must NOT
 *   depend on any pre-set gp: static non-PIE glibc/riscv64 crt1.S installs __global_pointer$
 *   itself as its very first act (`REGISTER_GP`), and PIE/ET_DYN images never use gp-relative
 *   addressing at all. If a future guest libc turned out to rely on the kernel planting `gp`
 *   before entry (the psABI allows either), this is the one place to grow that -- see the
 *   "still to verify" note in dev docs: riscv/PLAN.md.
 * - There is no PSTATE.NZCV to clear (riscv64 has no condition flags), and no FPCR vs FPSR
 *   split -- a single fcsr, whose reset value 0 (like aarch64's FPCR) already means "FP
 *   exceptions set flags but never trap", so no magic untrap constant like x86's 0x1F80 is
 *   needed.
 */

.text
.global call_elf_entry
.type call_elf_entry, %function

/* noreturn void call_elf_entry(elf_addr_t entry, void* argp) */
call_elf_entry:
    mv   sp, a1           /* initial stack: argc/argv/envp/auxv live there */
    csrw fcsr, zero       /* fcsr = 0: nearest analog of "FPCR/NZCV zeroed" */
    /* Zero every GPR except sp and t1 (which carries the jump target). */
    mv   t1, a0
    li   a0, 0
    li   ra, 0
    li   gp, 0
    li   tp, 0
    li   a1, 0
    li   a2, 0
    li   a3, 0
    li   a4, 0
    li   a5, 0
    li   a6, 0
    li   a7, 0
    li   t0, 0
    li   t2, 0
    li   s0, 0
    li   s1, 0
    li   s2, 0
    li   s3, 0
    li   s4, 0
    li   s5, 0
    li   s6, 0
    li   s7, 0
    li   s8, 0
    li   s9, 0
    li   s10, 0
    li   s11, 0
    li   t3, 0
    li   t4, 0
    li   t5, 0
    li   t6, 0
    jr   t1

.section .note.GNU-stack,"",%progbits
