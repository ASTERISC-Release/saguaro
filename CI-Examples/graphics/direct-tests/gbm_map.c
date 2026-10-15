/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/*
 * gbm_map: create GBM buffer objects on a render node, map them for the CPU, write a pattern,
 * unmap, map again and check it. Exercises Mesa's driver loading and the mapping of GEM objects
 * (mmap of the render node at the offset the driver hands out).
 * Usage: gbm_map [/dev/dri/renderD128]. Prints "TEST OK" on success.
 */

#include <errno.h>
#include <fcntl.h>
#include <gbm.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int check_bo(struct gbm_device* gbm, uint32_t width, uint32_t height, uint32_t flags,
                    const char* what) {
    struct gbm_bo* bo = gbm_bo_create(gbm, width, height, GBM_FORMAT_ARGB8888, flags);
    if (!bo) {
        printf("%s: gbm_bo_create failed: %s\n", what, strerror(errno));
        return 1;
    }

    for (int pass = 0; pass < 2; pass++) {
        uint32_t stride = 0;
        void* map_data = NULL;
        uint32_t* p = gbm_bo_map(bo, 0, 0, width, height, GBM_BO_TRANSFER_READ_WRITE, &stride,
                                 &map_data);
        if (!p) {
            printf("%s: gbm_bo_map failed: %s\n", what, strerror(errno));
            gbm_bo_destroy(bo);
            return 1;
        }
        for (uint32_t y = 0; y < height; y++) {
            uint32_t* row = (uint32_t*)((char*)p + (size_t)y * stride);
            for (uint32_t x = 0; x < width; x++) {
                uint32_t want = 0xff000000u | (y << 12) | x;
                if (pass == 0) {
                    row[x] = want;
                } else if (row[x] != want) {
                    printf("%s: mismatch at %u,%u: 0x%08x, expected 0x%08x\n", what, x, y, row[x],
                           want);
                    gbm_bo_unmap(bo, map_data);
                    gbm_bo_destroy(bo);
                    return 1;
                }
            }
        }
        gbm_bo_unmap(bo, map_data);
        if (pass == 0)
            printf("%s: %ux%u, stride %u, modifier 0x%llx, written\n", what, width, height, stride,
                   (unsigned long long)gbm_bo_get_modifier(bo));
    }
    gbm_bo_destroy(bo);
    printf("%s: read back OK\n", what);
    return 0;
}

int main(int argc, char** argv) {
    const char* node = argc > 1 ? argv[1] : "/dev/dri/renderD128";
    int fd = open(node, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        printf("open %s: %s\n", node, strerror(errno));
        return 1;
    }
    struct gbm_device* gbm = gbm_create_device(fd);
    if (!gbm) {
        printf("gbm_create_device failed\n");
        return 1;
    }
    printf("GBM backend %s\n", gbm_device_get_backend_name(gbm));

    int failed = 0;
    failed += check_bo(gbm, 64, 64, GBM_BO_USE_LINEAR, "linear 64x64");
    failed += check_bo(gbm, 1920, 1080, GBM_BO_USE_RENDERING, "render 1920x1080");
    failed += check_bo(gbm, 4096, 4096, GBM_BO_USE_LINEAR, "linear 4096x4096");

    gbm_device_destroy(gbm);
    close(fd);
    if (failed)
        return 1;
    printf("TEST OK\n");
    return 0;
}
