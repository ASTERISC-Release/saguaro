/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/*
 * A DRM render node under saguaro-direct: device number, the sysfs links libdrm reads, and ioctls
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
#include <poll.h>
#include <stdbool.h>
#include <linux/ioctl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <time.h>
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
struct drm_gem_close {
    uint32_t handle;
    uint32_t pad;
};
/* include/uapi/drm/i915_drm.h */
struct drm_i915_gem_create {
    uint64_t size;
    uint32_t handle;
    uint32_t pad;
};
struct drm_i915_gem_mmap_offset {
    uint32_t handle;
    uint32_t pad;
    uint64_t offset;
    uint64_t flags;
    uint64_t extensions;
};
#define I915_MMAP_OFFSET_WB 2
/* include/uapi/drm/amdgpu_drm.h */
union drm_amdgpu_gem_create {
    struct {
        uint64_t bo_size;
        uint64_t alignment;
        uint64_t domains;
        uint64_t domain_flags;
    } in;
    struct {
        uint32_t handle;
        uint32_t _pad;
    } out;
};
union drm_amdgpu_gem_mmap {
    struct {
        uint32_t handle;
        uint32_t _pad;
    } in;
    struct {
        uint64_t addr_ptr;
    } out;
};
#define AMDGPU_GEM_DOMAIN_GTT 0x2
#define AMDGPU_GEM_CREATE_CPU_ACCESS_REQUIRED (1 << 0)

struct drm_prime_handle {
    uint32_t handle;
    uint32_t flags;
    int32_t fd;
};
struct drm_syncobj_create {
    uint32_t handle;
    uint32_t flags;
};
struct drm_syncobj_destroy {
    uint32_t handle;
    uint32_t pad;
};
struct drm_syncobj_handle {
    uint32_t handle;
    uint32_t flags;
    int32_t fd;
    uint32_t pad;
    uint64_t point;
};

struct drm_syncobj_wait {
    uint64_t handles;
    int64_t timeout_nsec;
    uint32_t count_handles;
    uint32_t flags;
    uint32_t first_signaled;
    uint32_t pad;
    uint64_t deadline_nsec;
};
#define DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT (1 << 1)

#define DRM_IOCTL_BASE        'd'
#define DRM_IOCTL_VERSION     _IOWR(DRM_IOCTL_BASE, 0x00, struct drm_version)
#define DRM_IOCTL_GET_CAP     _IOWR(DRM_IOCTL_BASE, 0x0c, struct drm_get_cap)
#define DRM_IOCTL_GEM_CLOSE   _IOW(DRM_IOCTL_BASE, 0x09, struct drm_gem_close)
#define DRM_IOCTL_PRIME_HANDLE_TO_FD   _IOWR(DRM_IOCTL_BASE, 0x2d, struct drm_prime_handle)
#define DRM_IOCTL_PRIME_FD_TO_HANDLE   _IOWR(DRM_IOCTL_BASE, 0x2e, struct drm_prime_handle)
#define DRM_IOCTL_SYNCOBJ_CREATE       _IOWR(DRM_IOCTL_BASE, 0xbf, struct drm_syncobj_create)
#define DRM_IOCTL_SYNCOBJ_DESTROY      _IOWR(DRM_IOCTL_BASE, 0xc0, struct drm_syncobj_destroy)
#define DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD _IOWR(DRM_IOCTL_BASE, 0xc1, struct drm_syncobj_handle)
#define DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE _IOWR(DRM_IOCTL_BASE, 0xc2, struct drm_syncobj_handle)
#define DRM_IOCTL_SYNCOBJ_WAIT         _IOWR(DRM_IOCTL_BASE, 0xc3, struct drm_syncobj_wait)
#define DRM_CLOEXEC 02000000
#define DRM_RDWR    02
#define DRM_COMMAND_BASE      0x40
#define DRM_IOCTL_I915_GEM_CREATE \
    _IOWR(DRM_IOCTL_BASE, DRM_COMMAND_BASE + 0x1b, struct drm_i915_gem_create)
#define DRM_IOCTL_I915_GEM_MMAP_OFFSET \
    _IOWR(DRM_IOCTL_BASE, DRM_COMMAND_BASE + 0x24, struct drm_i915_gem_mmap_offset)
#define DRM_IOCTL_AMDGPU_GEM_CREATE \
    _IOWR(DRM_IOCTL_BASE, DRM_COMMAND_BASE + 0x00, union drm_amdgpu_gem_create)
#define DRM_IOCTL_AMDGPU_GEM_MMAP \
    _IOWR(DRM_IOCTL_BASE, DRM_COMMAND_BASE + 0x01, union drm_amdgpu_gem_mmap)
#define DRM_CAP_PRIME         0x5
#define DRM_MAJOR             226

#define BO_SIZE (64 * 4096)

/* Create a GEM buffer object with the driver's own ioctls and return the mmap offset of the
 * render node that maps it; false if the driver is not one of those known here. */
static bool create_bo(int fd, const char* driver, uint32_t* out_handle, uint64_t* out_offset) {
    if (!strcmp(driver, "i915")) {
        struct drm_i915_gem_create create = {.size = BO_SIZE};
        if (ioctl(fd, DRM_IOCTL_I915_GEM_CREATE, &create) < 0)
            err(1, "DRM_IOCTL_I915_GEM_CREATE");
        struct drm_i915_gem_mmap_offset mo = {.handle = create.handle,
                                              .flags = I915_MMAP_OFFSET_WB};
        if (ioctl(fd, DRM_IOCTL_I915_GEM_MMAP_OFFSET, &mo) < 0)
            err(1, "DRM_IOCTL_I915_GEM_MMAP_OFFSET");
        *out_handle = create.handle;
        *out_offset = mo.offset;
        return true;
    }
    if (!strcmp(driver, "amdgpu")) {
        union drm_amdgpu_gem_create create = {.in = {.bo_size = BO_SIZE, .alignment = 4096,
                                                     .domains = AMDGPU_GEM_DOMAIN_GTT,
                                                     .domain_flags =
                                                         AMDGPU_GEM_CREATE_CPU_ACCESS_REQUIRED}};
        if (ioctl(fd, DRM_IOCTL_AMDGPU_GEM_CREATE, &create) < 0)
            err(1, "DRM_IOCTL_AMDGPU_GEM_CREATE");
        union drm_amdgpu_gem_mmap gm = {.in = {.handle = create.out.handle}};
        if (ioctl(fd, DRM_IOCTL_AMDGPU_GEM_MMAP, &gm) < 0)
            err(1, "DRM_IOCTL_AMDGPU_GEM_MMAP");
        *out_handle = create.out.handle;
        *out_offset = gm.out.addr_ptr;
        return true;
    }
    return false;
}

/* fds that ioctls return and take: the LibOS must hand out its own fds for them */
static void check_prime(int fd, uint32_t handle) {
    struct drm_prime_handle ph = {.handle = handle, .flags = DRM_CLOEXEC | DRM_RDWR, .fd = -1};
    if (ioctl(fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &ph) < 0)
        err(1, "DRM_IOCTL_PRIME_HANDLE_TO_FD");
    int dmabuf = ph.fd;
    if (fcntl(dmabuf, F_GETFD) != FD_CLOEXEC)
        errx(1, "dma-buf fd %d: not a valid fd with FD_CLOEXEC", dmabuf);
    off_t size = lseek(dmabuf, 0, SEEK_END);
    if (size != BO_SIZE)
        errx(1, "dma-buf: lseek(SEEK_END) returned %lld, expected %d", (long long)size, BO_SIZE);
    if (lseek(dmabuf, 0, SEEK_SET) != 0)
        err(1, "dma-buf: lseek(SEEK_SET)");

    /* the dma-buf maps the same memory as the GEM object (word 18 was written above) */
    uint32_t* m = mmap(NULL, BO_SIZE, PROT_READ, MAP_SHARED, dmabuf, 0);
    if (m == MAP_FAILED)
        err(1, "mmap of the dma-buf");
    if (m[18] != 18u * 2654435761u)
        errx(1, "dma-buf mapping: wrong data");
    if (munmap(m, BO_SIZE) < 0)
        err(1, "munmap of the dma-buf");

    struct pollfd pfd = {.fd = dmabuf, .events = POLLIN};
    if (poll(&pfd, 1, 1000) != 1 || !(pfd.revents & POLLIN))
        errx(1, "poll on the idle dma-buf did not report POLLIN");
    int epfd = epoll_create1(EPOLL_CLOEXEC);
    struct epoll_event ev = {.events = EPOLLIN, .data.fd = dmabuf};
    if (epfd < 0 || epoll_ctl(epfd, EPOLL_CTL_ADD, dmabuf, &ev) < 0)
        err(1, "epoll on the dma-buf");
    if (epoll_wait(epfd, &ev, 1, 1000) != 1 || !(ev.events & EPOLLIN) || ev.data.fd != dmabuf)
        errx(1, "epoll_wait on the idle dma-buf did not report EPOLLIN");
    close(epfd);

    /* importing a duplicate fd gives the same GEM handle back */
    int dup_fd = dup(dmabuf);
    if (dup_fd < 0)
        err(1, "dup of the dma-buf");
    struct drm_prime_handle imp = {.fd = dup_fd};
    if (ioctl(fd, DRM_IOCTL_PRIME_FD_TO_HANDLE, &imp) < 0)
        err(1, "DRM_IOCTL_PRIME_FD_TO_HANDLE");
    if (imp.handle != handle || imp.fd != dup_fd)
        errx(1, "PRIME import: handle %u (expected %u), fd %d (expected %d)", imp.handle, handle,
             imp.fd, dup_fd);
    close(dup_fd);
    close(dmabuf);

    /* a syncobj exported as an fd and imported again */
    struct drm_syncobj_create sc = {0};
    if (ioctl(fd, DRM_IOCTL_SYNCOBJ_CREATE, &sc) < 0)
        err(1, "DRM_IOCTL_SYNCOBJ_CREATE");
    struct drm_syncobj_handle sh = {.handle = sc.handle, .fd = -1};
    if (ioctl(fd, DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD, &sh) < 0)
        err(1, "DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD");
    struct stat st;
    if (fstat(sh.fd, &st) < 0)
        err(1, "fstat of the syncobj fd");
    struct drm_syncobj_handle si = {.fd = sh.fd};
    if (ioctl(fd, DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE, &si) < 0)
        err(1, "DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE");
    if (si.handle == 0 || si.fd != sh.fd)
        errx(1, "syncobj import: handle %u, fd %d (expected %d)", si.handle, si.fd, sh.fd);
    close(sh.fd);
    /* an absolute CLOCK_MONOTONIC deadline 20 ms ahead (the kernel's clock is not Gramine's) */
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    int64_t start = (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
    struct drm_syncobj_wait wait = {.handles = (uintptr_t)&sc.handle, .count_handles = 1,
                                    .timeout_nsec = start + 20000000,
                                    .flags = DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT};
    if (ioctl(fd, DRM_IOCTL_SYNCOBJ_WAIT, &wait) != -1 || errno != ETIME)
        errx(1, "SYNCOBJ_WAIT on an empty syncobj: expected ETIME, got %s", strerror(errno));
    clock_gettime(CLOCK_MONOTONIC, &ts);
    int64_t waited_ms = ((int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec - start) / 1000000;
    if (waited_ms < 10 || waited_ms > 2000)
        errx(1, "SYNCOBJ_WAIT with a deadline 20 ms ahead returned after %lld ms",
             (long long)waited_ms);
    if (wait.timeout_nsec != start + 20000000)
        errx(1, "SYNCOBJ_WAIT: the deadline in the argument was not restored");

    struct drm_syncobj_destroy sd = {.handle = si.handle};
    if (ioctl(fd, DRM_IOCTL_SYNCOBJ_DESTROY, &sd) < 0)
        err(1, "DRM_IOCTL_SYNCOBJ_DESTROY");
    sd.handle = sc.handle;
    if (ioctl(fd, DRM_IOCTL_SYNCOBJ_DESTROY, &sd) < 0)
        err(1, "DRM_IOCTL_SYNCOBJ_DESTROY");

    printf("PRIME and syncobj fds: OK\n");
}

/* mmap of a GEM object through the render node: write, read back, mprotect, mremap, munmap */
static void check_gem_mapping(int fd, const char* driver) {
    uint32_t handle;
    uint64_t offset;
    if (!create_bo(fd, driver, &handle, &offset)) {
        printf("GEM mapping: driver %s not covered\n", driver);
        return;
    }

    uint32_t* p = mmap(NULL, BO_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)offset);
    if (p == MAP_FAILED)
        err(1, "mmap of the GEM object");
    for (size_t i = 0; i < BO_SIZE / sizeof(*p); i++)
        p[i] = (uint32_t)i * 2654435761u;

    /* a second mapping of the same object sees the data */
    uint32_t* q = mmap(NULL, BO_SIZE, PROT_READ, MAP_SHARED, fd, (off_t)offset);
    if (q == MAP_FAILED)
        err(1, "second mmap of the GEM object");
    for (size_t i = 0; i < BO_SIZE / sizeof(*q); i++)
        if (q[i] != (uint32_t)i * 2654435761u)
            errx(1, "GEM object: word %zu differs between the two mappings", i);
    if (munmap(q, BO_SIZE) < 0)
        err(1, "munmap");

    if (mprotect(p, BO_SIZE, PROT_READ) < 0)
        err(1, "mprotect(PROT_READ)");
    if (p[17] != 17u * 2654435761u)
        errx(1, "GEM object: wrong data after mprotect");
    if (mprotect(p, BO_SIZE, PROT_READ | PROT_WRITE) < 0)
        err(1, "mprotect(PROT_READ | PROT_WRITE)");
    p[17] = 0;

    /* shrink in place */
    uint32_t* r = mremap(p, BO_SIZE, BO_SIZE / 2, 0);
    if (r != p)
        err(1, "mremap (shrink)");
    if (p[18] != 18u * 2654435761u)
        errx(1, "GEM object: wrong data after mremap");
    if (munmap(p, BO_SIZE / 2) < 0)
        err(1, "munmap");

    printf("GEM mapping: %s OK\n", driver);

    check_prime(fd, handle);

    struct drm_gem_close gc = {.handle = handle};
    if (ioctl(fd, DRM_IOCTL_GEM_CLOSE, &gc) < 0)
        err(1, "DRM_IOCTL_GEM_CLOSE");
}

static void check_node(const char* node, bool denied) {
    int fd = open(node, O_RDWR | O_CLOEXEC);
    if (fd < 0)
        err(1, "open %s", node);

    struct drm_version v = {0};
    if (denied) {
        if (ioctl(fd, DRM_IOCTL_VERSION, &v) != -1 || errno != ENOSYS)
            errx(1, "%s: DRM_IOCTL_VERSION without the manifest key: expected ENOSYS, got %s",
                 node, strerror(errno));
        printf("%s: ioctl refused\n", node);
        close(fd);
        return;
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

    check_gem_mapping(fd, name);
    close(fd);
}

int main(int argc, char** argv) {
    bool denied = argc > 1 && !strcmp(argv[1], "denied");

    /* every render node of the host (the chroot mount of /dev/dri) */
    DIR* dir = opendir("/dev/dri");
    int nodes = 0;
    struct dirent* de;
    while (dir && (de = readdir(dir))) {
        if (strncmp(de->d_name, "renderD", strlen("renderD")))
            continue;
        char node[PATH_MAX];
        snprintf(node, sizeof(node), "/dev/dri/%s", de->d_name);
        check_node(node, denied);
        nodes++;
    }
    if (dir)
        closedir(dir);

    if (!nodes) {
        printf("SKIP: no render node\n");
        return 0;
    }
    printf("TEST OK\n");
    return 0;
}
