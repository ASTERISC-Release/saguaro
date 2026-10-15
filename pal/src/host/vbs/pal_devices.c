/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/* Devices of the vbs PAL proper: none (no host devices are reachable from a VBS enclave). */

#include "api.h"
#include "pal.h"
#include "pal_error.h"
#include "pal_internal.h"

static int dev_open(PAL_HANDLE* handle, const char* type, const char* uri, enum pal_access access,
                    pal_share_flags_t share, enum pal_create_mode create,
                    pal_stream_options_t options) {
    __UNUSED(handle);
    __UNUSED(type);
    __UNUSED(uri);
    __UNUSED(access);
    __UNUSED(share);
    __UNUSED(create);
    __UNUSED(options);
    return -PAL_ERROR_STREAMNOTEXIST;
}

static int dev_attrquery(const char* type, const char* uri, PAL_STREAM_ATTR* attr) {
    __UNUSED(type);
    __UNUSED(uri);
    __UNUSED(attr);
    return -PAL_ERROR_STREAMNOTEXIST;
}

struct handle_ops g_dev_ops = {
    .open      = &dev_open,
    .attrquery = &dev_attrquery,
};

int _PalDeviceIoControl(PAL_HANDLE handle, uint32_t cmd, unsigned long arg, int* out_ret) {
    __UNUSED(handle);
    __UNUSED(cmd);
    __UNUSED(arg);
    __UNUSED(out_ret);
    return -PAL_ERROR_NOTIMPLEMENTED;
}
