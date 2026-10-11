/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */
/* Copyright (C) 2023 Intel Corporation
 *                    Michał Kowalczyk <mkow@invisiblethingslab.com>
 */

#pragma once

/* Standard LP64 POSIX typedefs -- identical widths to x86_64/aarch64, reused verbatim. */

#include <stdint.h>

typedef unsigned int  uid_t;
typedef unsigned int  gid_t;
typedef int           pid_t;
typedef unsigned int  mode_t;
typedef long          off_t;
typedef long long     loff_t;
typedef uint32_t      dev_t;
typedef unsigned long ino_t;
typedef int           clockid_t;
