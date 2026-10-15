/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2023 Intel Corporation */

/*
 * Operations to handle devices. Currently VM PAL doesn't support any devices.
 */

#include "pal.h"
#include "pal_internal.h"
#include "pal_common.h"

#ifdef __x86_64__
/* `dev:/dev/shm/...` (Gramine's untrusted_shm mount): with the experimental multiprocess support,
 * objects of the guest store in guest RAM (pal_shm.c), shared by all guest processes; without it,
 * host files reached through virtio-fs (the opened handle is an ordinary file handle). */
static bool is_dev_shm(const char* uri) {
    return !strcmp(uri, "/dev/shm") || strstartswith(uri, "/dev/shm/");
}
#endif

static int dev_open(PAL_HANDLE* handle, const char* type, const char* uri, enum pal_access access,
                    pal_share_flags_t share, enum pal_create_mode create,
                    pal_stream_options_t options) {
#ifdef __x86_64__
    if (is_dev_shm(uri) && vm_shm_store_active() && vm_shm_is_object_uri(uri))
        return vm_shm_open(handle, uri, access, share, create);
    if (is_dev_shm(uri))
        return pal_common_file_open(handle, URI_TYPE_FILE, uri, access, share, create, options);
#endif
    __UNUSED(handle);
    __UNUSED(type);
    __UNUSED(uri);
    __UNUSED(access);
    __UNUSED(share);
    __UNUSED(create);
    __UNUSED(options);

    log_always("Device operations are not supported in VM PAL\n");
    return -PAL_ERROR_NOTIMPLEMENTED;
}

static int64_t dev_read(PAL_HANDLE handle, uint64_t offset, uint64_t size, void* buffer) {
#ifdef __x86_64__
    if (vm_shm_is_handle(handle))
        return vm_shm_read(handle, offset, size, buffer);
#endif
    __UNUSED(handle);
    __UNUSED(offset);
    __UNUSED(size);
    __UNUSED(buffer);
    return -PAL_ERROR_NOTIMPLEMENTED;
}

static int64_t dev_write(PAL_HANDLE handle, uint64_t offset, uint64_t size, const void* buffer) {
#ifdef __x86_64__
    if (vm_shm_is_handle(handle))
        return vm_shm_write(handle, offset, size, buffer);
#endif
    __UNUSED(handle);
    __UNUSED(offset);
    __UNUSED(size);
    __UNUSED(buffer);
    return -PAL_ERROR_NOTIMPLEMENTED;
}

static void dev_destroy(PAL_HANDLE handle) {
#ifdef __x86_64__
    if (vm_shm_is_handle(handle)) {
        vm_shm_destroy(handle);
        return;
    }
#endif
    __UNUSED(handle);
    /* noop */
}

static int dev_delete(PAL_HANDLE handle, enum pal_delete_mode delete_mode) {
#ifdef __x86_64__
    if (vm_shm_is_handle(handle))
        return delete_mode == PAL_DELETE_ALL ? vm_shm_delete(handle) : -PAL_ERROR_INVAL;
#endif
    __UNUSED(handle);
    __UNUSED(delete_mode);
    return -PAL_ERROR_NOTIMPLEMENTED;
}

static int dev_setlength(PAL_HANDLE handle, uint64_t length) {
#ifdef __x86_64__
    if (vm_shm_is_handle(handle))
        return vm_shm_setlength(handle, length);
#endif
    __UNUSED(handle);
    __UNUSED(length);
    return -PAL_ERROR_NOTIMPLEMENTED;
}

static int dev_flush(PAL_HANDLE handle) {
#ifdef __x86_64__
    if (vm_shm_is_handle(handle))
        return 0;
#endif
    __UNUSED(handle);
    return -PAL_ERROR_NOTIMPLEMENTED;
}

static int dev_attrquery(const char* type, const char* uri, PAL_STREAM_ATTR* attr) {
#ifdef __x86_64__
    if (is_dev_shm(uri) && vm_shm_store_active())
        return vm_shm_attrquery(uri, attr);
    if (is_dev_shm(uri))
        return pal_common_file_attrquery(URI_TYPE_FILE, uri, attr);
#endif
    __UNUSED(type);
    __UNUSED(uri);
    __UNUSED(attr);
    return -PAL_ERROR_NOTIMPLEMENTED;
}

static int dev_attrquerybyhdl(PAL_HANDLE handle, PAL_STREAM_ATTR* attr) {
#ifdef __x86_64__
    if (vm_shm_is_handle(handle))
        return vm_shm_attrquerybyhdl(handle, attr);
#endif
    __UNUSED(handle);
    __UNUSED(attr);
    return -PAL_ERROR_NOTIMPLEMENTED;
}

static int dev_map(PAL_HANDLE handle, void* addr, pal_prot_flags_t prot, uint64_t offset,
                   uint64_t size) {
#ifdef __x86_64__
    if (vm_shm_is_handle(handle))
        return vm_shm_map(handle, addr, prot, offset, size);
#endif
    __UNUSED(handle);
    __UNUSED(addr);
    __UNUSED(prot);
    __UNUSED(offset);
    __UNUSED(size);
    return -PAL_ERROR_NOTIMPLEMENTED;
}

struct handle_ops g_dev_ops = {
    .open           = &dev_open,
    .read           = &dev_read,
    .write          = &dev_write,
    .destroy        = &dev_destroy,
    .delete         = &dev_delete,
    .map            = &dev_map,
    .setlength      = &dev_setlength,
    .flush          = &dev_flush,
    .attrquery      = &dev_attrquery,
    .attrquerybyhdl = &dev_attrquerybyhdl,
};

int _PalDeviceIoControl(PAL_HANDLE handle, uint32_t cmd, unsigned long arg, int* out_ret) {
    __UNUSED(handle);
    __UNUSED(cmd);
    __UNUSED(arg);
    __UNUSED(out_ret);
    return -PAL_ERROR_NOTIMPLEMENTED;
}
