/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/*
 * drm_info: what Mesa's loader asks libdrm about a render node (version, PCI identity, caps).
 * Usage: drm_info [/dev/dri/renderD128]. Prints "TEST OK" when everything was answered.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#include <xf86drm.h>

int main(int argc, char** argv) {
    const char* node = argc > 1 ? argv[1] : "/dev/dri/renderD128";
    int failed = 0;

    int fd = open(node, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        printf("open %s: %s\n", node, strerror(errno));
        return 1;
    }

    struct stat st;
    if (fstat(fd, &st) < 0 || !S_ISCHR(st.st_mode)) {
        printf("fstat: not a character device\n");
        failed++;
    } else {
        printf("device %u:%u\n", major(st.st_rdev), minor(st.st_rdev));
    }

    drmVersionPtr v = drmGetVersion(fd);
    if (!v) {
        printf("drmGetVersion: %s\n", strerror(errno));
        failed++;
    } else {
        printf("driver %s %d.%d.%d (%s)\n", v->name, v->version_major, v->version_minor,
               v->version_patchlevel, v->desc);
        drmFreeVersion(v);
    }

    drmDevicePtr dev = NULL;
    int ret = drmGetDevice2(fd, 0, &dev);
    if (ret < 0) {
        printf("drmGetDevice2: %s\n", strerror(-ret));
        failed++;
    } else {
        if (dev->bustype == DRM_BUS_PCI)
            printf("pci %04x:%04x at %04x:%02x:%02x.%u\n", dev->deviceinfo.pci->vendor_id,
                   dev->deviceinfo.pci->device_id, dev->businfo.pci->domain,
                   dev->businfo.pci->bus, dev->businfo.pci->dev, dev->businfo.pci->func);
        else
            printf("bus type %d\n", dev->bustype);
        drmFreeDevice(&dev);
    }

    char* name = drmGetRenderDeviceNameFromFd(fd);
    printf("render node name %s\n", name ? name : "(none)");
    if (!name)
        failed++;

    uint64_t cap = 0;
    if (drmGetCap(fd, DRM_CAP_PRIME, &cap) < 0) {
        printf("drmGetCap(PRIME): %s\n", strerror(errno));
        failed++;
    } else {
        printf("prime caps 0x%llx\n", (unsigned long long)cap);
    }
    if (drmGetCap(fd, DRM_CAP_SYNCOBJ, &cap) < 0) {
        printf("drmGetCap(SYNCOBJ): %s\n", strerror(errno));
        failed++;
    } else {
        printf("syncobj %llu\n", (unsigned long long)cap);
    }

    drmDevicePtr devs[16];
    int n = drmGetDevices2(0, devs, 16);
    if (n < 0) {
        printf("drmGetDevices2: %s\n", strerror(-n));
        failed++;
    } else {
        printf("devices %d\n", n);
        drmFreeDevices(devs, n);
    }

    close(fd);
    if (failed) {
        printf("%d checks failed\n", failed);
        return 1;
    }
    printf("TEST OK\n");
    return 0;
}
