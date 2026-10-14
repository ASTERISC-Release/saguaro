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
