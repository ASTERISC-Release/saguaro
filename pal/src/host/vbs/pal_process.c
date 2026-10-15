/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 Gramine contributors */

/* Processes of the vbs PAL proper: one process per enclave; exit ends the boot (W's exit service
 * returns from E_PalMain to the host with the exit code). */

#include "api.h"
#include "pal.h"
#include "pal_error.h"
#include "pal_internal.h"
#include "pal_vbs.h"

int _PalProcessCreate(const char** args, uintptr_t (*reserved_mem_ranges)[2],
                      size_t reserved_mem_ranges_len, PAL_HANDLE* out_handle) {
    __UNUSED(args);
    __UNUSED(reserved_mem_ranges);
    __UNUSED(reserved_mem_ranges_len);
    __UNUSED(out_handle);
    return -PAL_ERROR_NOTIMPLEMENTED; /* no fork/exec of new enclaves */
}

noreturn void _PalProcessExit(int exitcode) {
    vbs_exit(exitcode);
}

struct handle_ops g_proc_ops = {
    /* nothing */
};
