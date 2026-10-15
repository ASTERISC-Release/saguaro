/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 Gramine contributors */

#include "api.h"
#include "pal.h"
#include "pal_error.h"
#include "pal_internal.h"
#include "pal_vbs.h"

int _PalSendHandle(PAL_HANDLE target_process, PAL_HANDLE cargo) {
    __UNUSED(target_process);
    __UNUSED(cargo);
    return -PAL_ERROR_NOTIMPLEMENTED;
}

int _PalReceiveHandle(PAL_HANDLE source_process, PAL_HANDLE* out_cargo) {
    __UNUSED(source_process);
    __UNUSED(out_cargo);
    return -PAL_ERROR_NOTIMPLEMENTED;
}

int _PalInitDebugStream(const char* path) {
    __UNUSED(path);
    return 0; /* the log always goes to the host console */
}

int _PalDebugLog(const void* buf, size_t size) {
    vbs_console_write(buf, size);
    return 0;
}
