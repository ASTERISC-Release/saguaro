/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/*
 * ioctl_bench: the cost of one DRM ioctl, natively and under saguaro-direct. Times a loop of
 * DRM_IOCTL_SYNCOBJ_WAIT with timeout 0 on a signaled syncobj (what Mesa's iris does to ask whether
 * a buffer is busy, thousands of times per frame in Quake II), DRM_IOCTL_GET_CAP, and getppid() as
 * the cost of a system call without the device.
 * Usage: ioctl_bench [/dev/dri/renderD128] [iterations]
 */

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#include <xf86drm.h>

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

int main(int argc, char** argv) {
    const char* node = argc > 1 ? argv[1] : "/dev/dri/renderD128";
    long iters = argc > 2 ? atol(argv[2]) : 200000;
    int fd = open(node, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        printf("open %s: %s\n", node, strerror(errno));
        return 1;
    }

    uint32_t syncobj;
    if (drmSyncobjCreate(fd, DRM_SYNCOBJ_CREATE_SIGNALED, &syncobj)) {
        printf("drmSyncobjCreate: %s\n", strerror(errno));
        return 1;
    }
    struct drm_syncobj_wait wait = {
        .handles = (uintptr_t)&syncobj, .count_handles = 1, .timeout_nsec = 0,
    };
    struct drm_get_cap cap = {.capability = DRM_CAP_PRIME};

    for (int round = 0; round < 2; round++) { /* the first round warms up */
        uint64_t t0 = now_ns();
        for (long i = 0; i < iters; i++)
            if (ioctl(fd, DRM_IOCTL_SYNCOBJ_WAIT, &wait)) {
                printf("SYNCOBJ_WAIT: %s\n", strerror(errno));
                return 1;
            }
        uint64_t t1 = now_ns();
        for (long i = 0; i < iters; i++)
            if (ioctl(fd, DRM_IOCTL_GET_CAP, &cap)) {
                printf("GET_CAP: %s\n", strerror(errno));
                return 1;
            }
        uint64_t t2 = now_ns();
        for (long i = 0; i < iters; i++)
            syscall(SYS_getppid);
        uint64_t t3 = now_ns();
        if (round == 1)
            printf("ns per call: SYNCOBJ_WAIT %.0f, GET_CAP %.0f, getppid %.0f (%ld iterations)\n",
                   (double)(t1 - t0) / iters, (double)(t2 - t1) / iters, (double)(t3 - t2) / iters,
                   iters);
    }
    drmSyncobjDestroy(fd, syncobj);
    close(fd);
    return 0;
}
