/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2014 Stony Brook University */

/*
 * This file contains APIs to open, read, write and get attributes of streams.
 */

#include "api.h"
#include "pal.h"
#include "pal_error.h"
#include "pal_internal.h"

#include "kernel_debug.h"
#include "kernel_virtio.h"

int _PalSendHandle(struct pal_handle* target_process, struct pal_handle* cargo) {
    return vm_process_send_handle(target_process, cargo);
}

int _PalReceiveHandle(struct pal_handle* source_process, struct pal_handle** out_cargo) {
    return vm_process_receive_handle(source_process, out_cargo);
}

int _PalInitDebugStream(const char* path) {
    __UNUSED(path); /* we print debug info to virtio-console currently, no files */
    return 0;
}

int _PalDebugLog(const void* buf, size_t size) {
    if (!g_console) {
        const char* buf_char = buf;
        for (size_t i = 0; i < size; i++)
            debug_serial_io_write_char(buf_char[i]);
        return 0;
    }

    return virtio_console_nprint(buf, size);
}
