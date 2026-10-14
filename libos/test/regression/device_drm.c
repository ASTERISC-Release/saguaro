/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/*
 * A DRM render node under gramine-direct: device number, the sysfs links libdrm reads, and ioctls
 * with pointers inside their argument (sys.insecure__allow_raw_device_ioctl). With the argument
 * "denied" it checks the default instead: without that manifest key the ioctl is refused.
 *
 * Prints "SKIP" when the host has no render node.
 */

#define _GNU_SOURCE
#include <dirent.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <linux/ioctl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

/* from the kernel's include/uapi/drm/drm.h, to not depend on its installation */
struct drm_version {
    int version_major;
    int version_minor;
    int version_patchlevel;
    size_t name_len;
    char* name;
    size_t date_len;
    char* date;
    size_t desc_len;
    char* desc;
};
struct drm_get_cap {
    uint64_t capability;
    uint64_t value;
};
#define DRM_IOCTL_BASE        'd'
#define DRM_IOCTL_VERSION     _IOWR(DRM_IOCTL_BASE, 0x00, struct drm_version)
#define DRM_IOCTL_GET_CAP     _IOWR(DRM_IOCTL_BASE, 0x0c, struct drm_get_cap)
#define DRM_CAP_PRIME         0x5
#define DRM_MAJOR             226

static int open_render_node(char* path, size_t size) {
    DIR* dir = opendir("/dev/dri");
    if (!dir)
        return -1;
    struct dirent* de;
    int fd = -1;
    while ((de = readdir(dir))) {
        if (strncmp(de->d_name, "renderD", strlen("renderD")))
            continue;
        if (de->d_type != DT_CHR)
            errx(1, "/dev/dri/%s: d_type %d, expected a character device", de->d_name, de->d_type);
        snprintf(path, size, "/dev/dri/%s", de->d_name);
        fd = open(path, O_RDWR | O_CLOEXEC);
        if (fd < 0)
            err(1, "open %s", path);
        break;
    }
    closedir(dir);
    return fd;
}

int main(int argc, char** argv) {
    bool denied = argc > 1 && !strcmp(argv[1], "denied");

    char node[PATH_MAX];
    int fd = open_render_node(node, sizeof(node));
    if (fd < 0) {
        printf("SKIP: no render node\n");
        return 0;
    }

    struct drm_version v = {0};
    if (denied) {
        if (ioctl(fd, DRM_IOCTL_VERSION, &v) != -1 || errno != ENOSYS)
            errx(1, "DRM_IOCTL_VERSION without the manifest key: expected ENOSYS, got %s",
                 strerror(errno));
        printf("TEST OK\n");
        return 0;
    }

    struct stat st;
    if (fstat(fd, &st) < 0)
        err(1, "fstat %s", node);
    if (!S_ISCHR(st.st_mode) || major(st.st_rdev) != DRM_MAJOR)
        errx(1, "%s: mode 0%o, device %u:%u", node, st.st_mode, major(st.st_rdev),
             minor(st.st_rdev));

    /* libdrm finds the bus type through this link (`host_symlinks` on the /sys mount) */
    char link[PATH_MAX], target[PATH_MAX];
    snprintf(link, sizeof(link), "/sys/dev/char/%u:%u/device/subsystem", major(st.st_rdev),
             minor(st.st_rdev));
    ssize_t n = readlink(link, target, sizeof(target) - 1);
    if (n < 0)
        err(1, "readlink %s", link);
    target[n] = '\0';
    if (!strstr(target, "bus/"))
        errx(1, "readlink %s: unexpected target %s", link, target);

    /* first call: lengths; second call: the kernel writes through the pointers in the argument */
    if (ioctl(fd, DRM_IOCTL_VERSION, &v) < 0)
        err(1, "DRM_IOCTL_VERSION (lengths)");
    if (v.name_len == 0 || v.name_len > 64)
        errx(1, "DRM_IOCTL_VERSION: name_len %zu", v.name_len);
    char name[65] = {0}, date[65] = {0}, desc[257] = {0};
    v.name = name;
    v.date = date;
    v.desc = desc;
    v.date_len = v.date_len < 64 ? v.date_len : 64;
    v.desc_len = v.desc_len < 256 ? v.desc_len : 256;
    if (ioctl(fd, DRM_IOCTL_VERSION, &v) < 0)
        err(1, "DRM_IOCTL_VERSION");
    if (strlen(name) == 0)
        errx(1, "DRM_IOCTL_VERSION: empty driver name");

    struct drm_get_cap cap = {.capability = DRM_CAP_PRIME};
    if (ioctl(fd, DRM_IOCTL_GET_CAP, &cap) < 0)
        err(1, "DRM_IOCTL_GET_CAP");

    /* an ioctl the driver does not know: the host's error, not ENOSYS */
    if (ioctl(fd, _IO(DRM_IOCTL_BASE, 0xff), 0) != -1 || errno == ENOSYS)
        errx(1, "unknown DRM ioctl: expected a host error, got %s", strerror(errno));

    printf("%s: driver %s, subsystem %s, PRIME caps 0x%llx\n", node, name, strrchr(target, '/') + 1,
           (unsigned long long)cap.value);
    close(fd);
    printf("TEST OK\n");
    return 0;
}
