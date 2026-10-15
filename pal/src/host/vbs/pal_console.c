/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 Gramine contributors */

/* Console of the vbs PAL proper: writes go to the host console through W's console OCALL; there is
 * no console input inside the enclave (reads return end of file). */

#include "api.h"
#include "pal.h"
#include "pal_error.h"
#include "pal_internal.h"
#include "pal_vbs.h"

static int console_open(PAL_HANDLE* handle, const char* type, const char* uri,
                        enum pal_access access, pal_share_flags_t share,
                        enum pal_create_mode create, pal_stream_options_t options) {
    __UNUSED(uri);
    __UNUSED(share);
    __UNUSED(create);
    __UNUSED(options);
    if (strcmp(type, URI_TYPE_CONSOLE))
        return -PAL_ERROR_INVAL;
    PAL_HANDLE hdl = calloc(1, HANDLE_SIZE(console));
    if (!hdl)
        return -PAL_ERROR_NOMEM;
    init_handle_hdr(hdl, PAL_TYPE_CONSOLE);
    hdl->console.writable = access != PAL_ACCESS_RDONLY;
    *handle = hdl;
    return 0;
}

static int64_t console_read(PAL_HANDLE handle, uint64_t offset, uint64_t size, void* buffer) {
    __UNUSED(handle);
    __UNUSED(offset);
    __UNUSED(size);
    __UNUSED(buffer);
    return 0;
}

static int64_t console_write(PAL_HANDLE handle, uint64_t offset, uint64_t size,
                             const void* buffer) {
    __UNUSED(handle);
    __UNUSED(offset);
    vbs_console_write(buffer, size);
    return (int64_t)size;
}

static void console_destroy(PAL_HANDLE handle) {
    free(handle);
}

static int console_flush(PAL_HANDLE handle) {
    __UNUSED(handle);
    return 0;
}

static int console_attrquerybyhdl(PAL_HANDLE handle, PAL_STREAM_ATTR* attr) {
    __UNUSED(handle);
    memset(attr, 0, sizeof(*attr));
    attr->handle_type = PAL_TYPE_CONSOLE;
    attr->share_flags = 0666;
    return 0;
}

struct handle_ops g_console_ops = {
    .open           = &console_open,
    .read           = &console_read,
    .write          = &console_write,
    .destroy        = &console_destroy,
    .flush          = &console_flush,
    .attrquerybyhdl = &console_attrquerybyhdl,
};
