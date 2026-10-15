/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/*
 * eventfd of the vbs PAL proper: an in-enclave 64-bit counter (one process per enclave, so there is
 * no other process to share it with), with Linux's semantics: a read returns the counter (or 1 in
 * semaphore mode) and lowers it, blocking while it is 0; a write adds a value, blocking while the
 * sum would exceed 0xfffffffffffffffe. Waiters sleep on the stream-change counter (pal_object.c),
 * so poll/epoll see eventfds together with pipes and sockets.
 */

#include "api.h"
#include "pal.h"
#include "pal_error.h"
#include "pal_internal.h"
#include "pal_vbs.h"
#include "spinlock.h"

#define EFD_MAX 0xfffffffffffffffeull

struct vbs_eventfd {
    spinlock_t lock;
    uint64_t counter;
    bool semaphore;
};

static int eventfd_pal_open(PAL_HANDLE* handle, const char* type, const char* uri,
                            enum pal_access access, pal_share_flags_t share,
                            enum pal_create_mode create, pal_stream_options_t options) {
    __UNUSED(uri);
    __UNUSED(access);
    __UNUSED(share);
    __UNUSED(create);
    if (strcmp(type, URI_TYPE_EVENTFD))
        return -PAL_ERROR_INVAL;
    PAL_HANDLE hdl = calloc(1, HANDLE_SIZE(eventfd));
    struct vbs_eventfd* efd = calloc(1, sizeof(*efd));
    if (!hdl || !efd) {
        free(hdl);
        free(efd);
        return -PAL_ERROR_NOMEM;
    }
    spinlock_init(&efd->lock);
    efd->semaphore = !!(options & PAL_OPTION_EFD_SEMAPHORE);
    init_handle_hdr(hdl, PAL_TYPE_EVENTFD);
    hdl->eventfd.efd = efd;
    hdl->eventfd.nonblocking = !!(options & PAL_OPTION_NONBLOCK);
    *handle = hdl;
    return 0;
}

static int64_t eventfd_pal_read(PAL_HANDLE handle, uint64_t offset, uint64_t len, void* buffer) {
    __UNUSED(offset);
    if (len < sizeof(uint64_t))
        return -PAL_ERROR_INVAL;
    struct vbs_eventfd* efd = handle->eventfd.efd;
    while (1) {
        uint32_t gen = __atomic_load_n(&g_vbs_stream_gen, __ATOMIC_ACQUIRE);
        spinlock_lock(&efd->lock);
        if (efd->counter) {
            uint64_t v = efd->semaphore ? 1 : efd->counter;
            efd->counter -= v;
            spinlock_unlock(&efd->lock);
            memcpy(buffer, &v, sizeof(v));
            vbs_streams_changed(); /* room for a blocked writer */
            return sizeof(v);
        }
        spinlock_unlock(&efd->lock);
        if (handle->eventfd.nonblocking)
            return -PAL_ERROR_TRYAGAIN;
        vbs_wait(&g_vbs_stream_gen, gen, UINT64_MAX);
    }
}

static int64_t eventfd_pal_write(PAL_HANDLE handle, uint64_t offset, uint64_t len,
                                 const void* buffer) {
    __UNUSED(offset);
    if (len < sizeof(uint64_t))
        return -PAL_ERROR_INVAL;
    uint64_t v;
    memcpy(&v, buffer, sizeof(v));
    if (v == UINT64_MAX)
        return -PAL_ERROR_INVAL;
    struct vbs_eventfd* efd = handle->eventfd.efd;
    while (1) {
        uint32_t gen = __atomic_load_n(&g_vbs_stream_gen, __ATOMIC_ACQUIRE);
        spinlock_lock(&efd->lock);
        if (efd->counter <= EFD_MAX - v) {
            efd->counter += v;
            spinlock_unlock(&efd->lock);
            vbs_streams_changed();
            return sizeof(v);
        }
        spinlock_unlock(&efd->lock);
        if (handle->eventfd.nonblocking)
            return -PAL_ERROR_TRYAGAIN;
        vbs_wait(&g_vbs_stream_gen, gen, UINT64_MAX);
    }
}

pal_wait_flags_t vbs_eventfd_ready(PAL_HANDLE handle) {
    struct vbs_eventfd* efd = handle->eventfd.efd;
    spinlock_lock(&efd->lock);
    pal_wait_flags_t r = (efd->counter ? PAL_WAIT_READ : 0) |
                         (efd->counter < EFD_MAX ? PAL_WAIT_WRITE : 0);
    spinlock_unlock(&efd->lock);
    return r;
}

static int eventfd_pal_attrquerybyhdl(PAL_HANDLE handle, PAL_STREAM_ATTR* attr) {
    memset(attr, 0, sizeof(*attr));
    attr->handle_type = PAL_TYPE_EVENTFD;
    attr->nonblocking = handle->eventfd.nonblocking;
    attr->pending_size = (vbs_eventfd_ready(handle) & PAL_WAIT_READ) ? sizeof(uint64_t) : 0;
    return 0;
}

static int eventfd_pal_attrsetbyhdl(PAL_HANDLE handle, PAL_STREAM_ATTR* attr) {
    handle->eventfd.nonblocking = attr->nonblocking;
    return 0;
}

static void eventfd_pal_destroy(PAL_HANDLE handle) {
    free(handle->eventfd.efd);
    free(handle);
}

struct handle_ops g_eventfd_ops = {
    .open           = &eventfd_pal_open,
    .read           = &eventfd_pal_read,
    .write          = &eventfd_pal_write,
    .destroy        = &eventfd_pal_destroy,
    .attrquerybyhdl = &eventfd_pal_attrquerybyhdl,
    .attrsetbyhdl   = &eventfd_pal_attrsetbyhdl,
};
