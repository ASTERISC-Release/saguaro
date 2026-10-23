# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The ASTERISC Research Lab

# The x86_64 ABI conformance suite here (libos/test/abi/x86_64/) pins down x86_64 process-entry
# register state (RFLAGS, MXCSR, FPU control word, ...) via hand-written NASM. Deferred for
# aarch64 per the Milestone A plan: worth an AAPCS64-entry-state counterpart eventually, but not
# required to reach the helloworld milestone this port is targeting first. This file exists only
# to satisfy the unconditional `subdir(host_machine.cpu_family())` in libos/test/abi/meson.build;
# it intentionally defines no test targets.
