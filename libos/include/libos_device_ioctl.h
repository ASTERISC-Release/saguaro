/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/*
 * The device ioctls whose argument carries file descriptors or absolute deadlines, and where in the
 * argument they are. Used by the raw device ioctls of gramine-direct (fs/hostfd/fs.c) and by the
 * forwarded ioctls of the VM graphics redirection (sys/libos_ioctl.c), which both translate those
 * fields between the application's view (LibOS fds, Gramine's clock) and the host's.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "linux_abi/ioctl.h"

/* The ioctls below, from the kernel's uapi headers (drm/drm.h, drm/amdgpu_drm.h, linux/dma-buf.h,
 * linux/sync_file.h); the sizes are those of the argument structures. */
#define DRM_IOCTL_PRIME_HANDLE_TO_FD    _IOC(_IOC_READ | _IOC_WRITE, 'd', 0x2d, 12)
#define DRM_IOCTL_PRIME_FD_TO_HANDLE    _IOC(_IOC_READ | _IOC_WRITE, 'd', 0x2e, 12)
#define DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD  _IOC(_IOC_READ | _IOC_WRITE, 'd', 0xc1, 24)
#define DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE  _IOC(_IOC_READ | _IOC_WRITE, 'd', 0xc2, 24)
#define DRM_IOCTL_SYNCOBJ_WAIT          _IOC(_IOC_READ | _IOC_WRITE, 'd', 0xc3, 40)
#define DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT _IOC(_IOC_READ | _IOC_WRITE, 'd', 0xca, 48)
#define DRM_IOCTL_AMDGPU_WAIT_CS        _IOC(_IOC_READ | _IOC_WRITE, 'd', 0x49, 32)
#define DRM_IOCTL_AMDGPU_WAIT_FENCES    _IOC(_IOC_READ | _IOC_WRITE, 'd', 0x52, 24)
#define DMA_BUF_IOCTL_EXPORT_SYNC_FILE  _IOC(_IOC_READ | _IOC_WRITE, 'b', 2, 8)
#define DMA_BUF_IOCTL_IMPORT_SYNC_FILE  _IOC(_IOC_WRITE, 'b', 3, 8)
#define SYNC_IOC_MERGE                  _IOC(_IOC_READ | _IOC_WRITE, '>', 3, 48)

/* The O_CLOEXEC flag of struct drm_prime_handle (DRM_CLOEXEC) */
#define DRM_CLOEXEC 02000000

enum field_kind {
    FD_IN,     /* int32_t LibOS fd, read by the kernel */
    FD_OUT,    /* int32_t host fd, written by the kernel */
    /* int64_t absolute CLOCK_MONOTONIC deadline in ns, read by the kernel. The application computed
     * it from Gramine's clock (real time), so it is moved to the host's clock for the call. */
    TIME_ABS,
};

struct ioctl_field {
    uint16_t offset;
    enum field_kind kind;
};

struct translated_ioctl {
    unsigned int cmd;
    struct ioctl_field fields[2];
    size_t fields_cnt;
    int16_t cloexec_flags_offset; /* offset of a flags word with DRM_CLOEXEC, or -1: always cloexec */
};

static const struct translated_ioctl g_translated_ioctls[] = {
    /* struct drm_prime_handle { u32 handle; u32 flags; s32 fd; } */
    { DRM_IOCTL_PRIME_HANDLE_TO_FD,    {{8, FD_OUT}}, 1, 4 },
    { DRM_IOCTL_PRIME_FD_TO_HANDLE,    {{8, FD_IN}},  1, -1 },
    /* struct drm_syncobj_handle { u32 handle; u32 flags; s32 fd; u32 pad; u64 point; } */
    { DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD,  {{8, FD_OUT}}, 1, -1 },
    { DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE,  {{8, FD_IN}},  1, -1 },
    /* struct drm_syncobj_wait { u64 handles; s64 timeout_nsec; u32 count_handles; u32 flags;
     *                           u32 first_signaled; u32 pad; u64 deadline_nsec; } */
    { DRM_IOCTL_SYNCOBJ_WAIT,          {{8, TIME_ABS}}, 1, -1 },
    /* struct drm_syncobj_timeline_wait { u64 handles; u64 points; s64 timeout_nsec; ... } */
    { DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT, {{16, TIME_ABS}}, 1, -1 },
    /* union drm_amdgpu_wait_cs { struct { u64 handle; u64 timeout; ... } in; ... } */
    { DRM_IOCTL_AMDGPU_WAIT_CS,        {{8, TIME_ABS}}, 1, -1 },
    /* union drm_amdgpu_wait_fences { struct { u64 fences; u32 count; u32 wait_all; u64 timeout_ns; }
     *                                in; ... } */
    { DRM_IOCTL_AMDGPU_WAIT_FENCES,    {{16, TIME_ABS}}, 1, -1 },
    /* struct dma_buf_export_sync_file { u32 flags; s32 fd; } (and the import one) */
    { DMA_BUF_IOCTL_EXPORT_SYNC_FILE,  {{4, FD_OUT}}, 1, -1 },
    { DMA_BUF_IOCTL_IMPORT_SYNC_FILE,  {{4, FD_IN}},  1, -1 },
    /* struct sync_merge_data { char name[32]; s32 fd2; s32 fence; u32 flags; u32 pad; } */
    { SYNC_IOC_MERGE,                  {{32, FD_IN}, {36, FD_OUT}}, 2, -1 },
};

static inline const struct translated_ioctl* find_translated_ioctl(unsigned int cmd) {
    for (size_t i = 0; i < sizeof(g_translated_ioctls) / sizeof(g_translated_ioctls[0]); i++)
        if (g_translated_ioctls[i].cmd == cmd)
            return &g_translated_ioctls[i];
    return NULL;
}
