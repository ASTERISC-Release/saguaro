/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */
/* Copyright (C) 2022 Intel Corporation
 *                    Mariusz Zaborski <oshogbo@invisiblethingslab.com> */

/* GAS port of libos_elf_entry.nasm -- NASM has no aarch64 backend.
 *
 * Before jumping to the entry point, we set:
 * - X0..X8: zeroed. The kernel's execve enters the program with *all* GPRs zero, and this is
 *   load-bearing on aarch64: glibc's `_start` begins with `mov x5, x0` -- the incoming x0 *is*
 *   the `rtld_fini` argument to __libc_start_main (real aarch64 ld.so passes its DT_FINI there;
 *   the kernel guarantees 0 for statically-linked programs that start without an interpreter).
 *   Entering with x0 = the entry address made glibc register `_start` itself as an atexit
 *   handler: on exit() it re-ran __libc_start_main after RELRO had been applied, faulting on
 *   the __libc_stack_end store (the bootstrap_static regression test). X16/X17 carry the jump
 *   targets instead -- they are link-time scratch registers the entry code may not observe.
 * - X2 zeroing is kept from the old port for symmetry with x86_64's RDX=0 "no atexit function"
 *   convention (subsumed by the full zeroing above).
 * - SP: the initial stack, contains program arguments and environment.
 * - PSTATE.NZCV: zeroed (AArch64's equivalent of clearing the low FLAGS bits).
 * - FPCR: zeroed. Unlike x86_64's MXCSR (whose architectural reset value traps on every FP
 *   exception, hence needing an explicit 0x1F80 to mask them), AArch64's FPCR reset value of 0
 *   already means "exceptions set FPSR status flags but never trap" -- the behavior x86_64 has
 *   to ask for explicitly is simply FPCR's default here, so there's no equivalent of the magic
 *   0x1F80 constant to encode.
 */

.text
.global call_elf_entry
.type call_elf_entry, %function

/* noreturn void call_elf_entry(elf_addr_t entry, void* argp) */
call_elf_entry:
    mov  x16, x0      /* entry (IP0/IP1: ABI-scratch, invisible to the program's startup code) */
    mov  x17, x1
    mov  x0, #0
    mov  x1, #0
    mov  x2, #0
    mov  x3, #0
    mov  x4, #0
    mov  x5, #0
    mov  x6, #0
    mov  x7, #0
    mov  x8, #0
    msr  fpcr, x2
    msr  nzcv, x2
#ifdef PAL_HOST_VM
    /* VM-PAL (aarch64 Milestone B, PAL_HOST_VM is defined only in the vm build): we run this
     * jump-once from pal_start_continue at EL1 (start_execution `br`'d here from kernel context,
     * exactly like x86's `jmp rdi` in ring 0), and this is the kernel's deliberate, one-way
     * privilege drop into userland. x86 gets its first ring-3 transition for free: ring-0
     * `syscall` still vectoring through LSTAR plus `sysretq`'s unconditional CPL=3 return
     * (sysret_asm) quietly drop the app to ring 3 after its first syscall. aarch64's `svc`
     * executed at EL1 is architecturally undefined -- it never reaches the EL0 vector table --
     * so the transition must be explicit here, at the exact point where the final user stack
     * (argp) is installed. eret to EL0 does not reload any GPR and does not restore SP -- an
     * EL0 target runs on SP_EL0, which must therefore be written through its sysreg directly.
     * SPSR = 0: EL0t with DAIF unmasked (x86 parity: sysret_asm's `or $0x200, %r11`). SP_EL1
     * intentionally keeps the stale kernel-side value: every aarch64 syscall entry (el0_sync)
     * reloads SP from SP_EL0, so nothing depends on it across the drop. */
    msr  sp_el0, x17  /* the app's initial stack (eret won't set it for an EL0 target) */
    msr  elr_el1, x16
    msr  spsr_el1, xzr
    eret              /* now at EL0: subsequent `svc` trap to el0_sync_entry */
#else
    mov  sp, x17      /* set stack pointer to second arg */
    br   x16          /* jump to entry point */
#endif

.section .note.GNU-stack,"",%progbits
