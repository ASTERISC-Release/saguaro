/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/*
 * Host passthrough calls (see pal.h). Only the Linux PAL implements them; the weak defaults below
 * serve every other PAL, so that they need no change.
 */

#include "pal.h"
#include "pal_error.h"
#include "pal_internal.h"

__attribute__((weak)) int _PalHostReadlink(const char* uri, char* buf, size_t* size) {
    __UNUSED(uri);
    __UNUSED(buf);
    __UNUSED(size);
    return -PAL_ERROR_NOTIMPLEMENTED;
}

int PalHostReadlink(const char* typed_uri, char* buf, size_t* size) {
    return _PalHostReadlink(typed_uri, buf, size);
}

__attribute__((weak)) int _PalHostFdWrap(int host_fd, PAL_HANDLE* out_handle) {
    __UNUSED(host_fd);
    __UNUSED(out_handle);
    return -PAL_ERROR_NOTIMPLEMENTED;
}

int PalHostFdWrap(int host_fd, PAL_HANDLE* out_handle) {
    return _PalHostFdWrap(host_fd, out_handle);
}

__attribute__((weak)) int _PalHostFdGet(PAL_HANDLE handle, int* out_host_fd) {
    __UNUSED(handle);
    __UNUSED(out_host_fd);
    return -PAL_ERROR_NOTIMPLEMENTED;
}

int PalHostFdGet(PAL_HANDLE handle, int* out_host_fd) {
    return _PalHostFdGet(handle, out_host_fd);
}
