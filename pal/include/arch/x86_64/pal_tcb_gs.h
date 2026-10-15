/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/*
 * Where the PAL_TCB lies relative to the %gs base on x86_64 (included by C and assembly).
 *
 * Every PAL keeps the PAL_TCB at %gs:0 -- except the VBS-enclave PAL: inside a VBS enclave %gs is
 * the Windows TEB of the thread, which Windows code needs, so the PAL_TCB lives in a part of the TEB
 * that an enclave never uses (Documentation/devel/vbs-enclave/PLAN-x64.md, X2; the enclave TEB is
 * 8 KiB and nearly empty, FINDINGS-x64.md). The Windows enclave runtime uses the same value
 * (PAL_VBS_TEB_TCB_OFFSET in pal/src/host/vbs/enclave/pal_vbs_boot.c).
 */

#pragma once

#ifdef PAL_HOST_VBS
#define PAL_TCB_GS_OFFSET 0x800
#else
#define PAL_TCB_GS_OFFSET 0
#endif
