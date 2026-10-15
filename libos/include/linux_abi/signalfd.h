/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/*
 * Linux UAPI types of "signalfd"/"signalfd4". Binary-identical to the kernel headers, including
 * the padding to 128 bytes (`sizeof(struct signalfd_siginfo)` is fixed by the ABI since Linux
 * 4.4).
 */

#pragma once

#include <stdint.h>

#define SFD_NONBLOCK 00004000 /* O_NONBLOCK */
#define SFD_CLOEXEC  02000000 /* O_CLOEXEC */

struct signalfd_siginfo {
    uint32_t ssi_signo;
    int32_t  ssi_errno;
    int32_t  ssi_code;
    uint32_t ssi_pid;
    uint32_t ssi_uid;
    int32_t  ssi_fd;
    uint32_t ssi_tid;
    uint32_t ssi_band;
    uint32_t ssi_overrun;
    uint32_t ssi_trapno;
    int32_t  ssi_status;
    int32_t  ssi_int;
    uint64_t ssi_ptr;
    uint64_t ssi_utime;
    uint64_t ssi_stime;
    uint64_t ssi_addr;
    uint16_t ssi_addr_lsb;
    uint16_t __pad2;
    int32_t  ssi_syscall;
    uint64_t ssi_call_addr;
    uint32_t ssi_arch;
    uint8_t  __pad[28];
};

static_assert(sizeof(struct signalfd_siginfo) == 128, "signalfd_siginfo size");
