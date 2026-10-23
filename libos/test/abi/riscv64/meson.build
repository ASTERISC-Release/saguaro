# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The ASTERISC Research Lab

# The x86_64 ABI conformance suite here (libos/test/abi/x86_64/) pins down x86_64 process-entry
# register state (RFLAGS, MXCSR, FPU control word, ...) via hand-written NASM. Deferred for
# riscv64: the interesting entry-state contract there is the one call_elf_entry() establishes
# (all GPRs zero but sp and the jump target, fcsr cleared, 16-byte-aligned SP, and -- open
# question -- `gp` zero vs __global_pointer$; see the comment in
# libos/src/arch/riscv64/libos_elf_entry.S) and it deserves an AAPCS-FP/riscv entry-state
# counterpart test in Phase 5+ of dev docs: riscv/PLAN.md, not a rushed translation.
# This file exists only to satisfy the unconditional `subdir(host_machine.cpu_family())` in
# libos/test/abi/meson.build; it intentionally defines no test targets.
