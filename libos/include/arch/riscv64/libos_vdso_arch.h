/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

#pragma once

/* Same vDSO filename on riscv64 as x86_64/aarch64 (the generic name Linux's ELF_PT_GNU_PROPERTY
 * independent ELF aux-vector entry uses on all of them). */
#define LINUX_VDSO_FILENAME "linux-vdso.so.1"
