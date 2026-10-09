/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

#pragma once

/* Same vDSO filename on aarch64 as x86_64 (32-bit ARM uses a different one, but that's not
 * relevant to this LP64 port). */
#define LINUX_VDSO_FILENAME "linux-vdso.so.1"
