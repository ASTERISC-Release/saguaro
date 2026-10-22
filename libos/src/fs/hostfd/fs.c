/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/*
 * Host file descriptors without a path, and raw device ioctls (saguaro-direct only).
 *
 * With `sys.insecure__allow_raw_device_ioctl`, the Linux PAL passes device ioctls to the host with
 * the application's argument (see `_PalDeviceIoControl()`). Some ioctls carry file descriptors
 * inside their argument: DRM PRIME and syncobj export/import, dma-buf sync files. Those are host
 * fds on the host side and LibOS fds on the application side, so this file translates them:
 * an input LibOS fd is replaced by the host fd of its handle for the duration of the call, and an
 * output host fd is wrapped into a new `TYPE_HOSTFD` handle whose LibOS fd is written back.
 * Absolute `CLOCK_MONOTONIC` deadlines (syncobj and amdgpu waits) are moved from Saguaro's clock,
 * which is real time, to the host's monotonic clock: otherwise a "has it signaled?" query with a
 * deadline of "now" turns into a wait until the GPU is done.
 *
 * A `TYPE_HOSTFD` handle owns a PAL device handle of the host fd (`PalHostFdWrap()`). It supports
 * read, write, mmap, fstat, lseek (for the size of a dma-buf), poll (generic, through the PAL
 * handle), and ioctl (raw, with the same translation).
 */

#include "libos_flags_conv.h"
#include "libos_fs.h"
#include "libos_handle.h"
#include "libos_internal.h"
#include "libos_lock.h"
#include "linux_abi/errors.h"
#include "linux_abi/fs.h"
#include "linux_abi/ioctl.h"
#include "pal.h"
#include "stat.h"
#include "toml_utils.h"

/* the DRM, dma-buf and sync_file table, shared with the VM graphics path (sys/libos_ioctl.c) */
#include "libos_device_ioctl.h"

/* The NVIDIA proprietary driver's ioctls on /dev/nvidiactl and /dev/nvidia<N> (character major 195)
 * that carry an fd of the caller in their argument, from nv-ioctl-numbers.h, nv-ioctl.h and the
 * driver's "with fd" wrappers of NVOS33/NVOS02 (offsets checked against a native run of driver
 * 580.178.04). The kernel looks the fd up with fget() in the calling process, so it must be the
 * host fd. */
#define NV_MAJOR_DEVICE_NUMBER    195
#define NV_IOCTL_MAGIC            'F'
#define NV_ESC_RM_ALLOC_MEMORY    _IOC(_IOC_READ | _IOC_WRITE, NV_IOCTL_MAGIC, 0x27, 0x38)
#define NV_ESC_RM_MAP_MEMORY      _IOC(_IOC_READ | _IOC_WRITE, NV_IOCTL_MAGIC, 0x4e, 0x38)
#define NV_ESC_REGISTER_FD        _IOC(_IOC_READ | _IOC_WRITE, NV_IOCTL_MAGIC, 0xc9, 4)
#define NV_ESC_ALLOC_OS_EVENT     _IOC(_IOC_READ | _IOC_WRITE, NV_IOCTL_MAGIC, 0xce, 16)
#define NV_ESC_FREE_OS_EVENT      _IOC(_IOC_READ | _IOC_WRITE, NV_IOCTL_MAGIC, 0xcf, 16)


/* Only for handles of NVIDIA devices (character major 195): the numbers are not unique. */
static const struct translated_ioctl g_translated_nvidia_ioctls[] = {
    /* struct { NVOS02_PARAMETERS params; int fd; } (fd may be -1) */
    { NV_ESC_RM_ALLOC_MEMORY, {{0x30, FD_IN}}, 1, -1 },
    /* struct { NVOS33_PARAMETERS params; int fd; }: the fd that is mmap()ed afterwards */
    { NV_ESC_RM_MAP_MEMORY,   {{0x30, FD_IN}}, 1, -1 },
    /* struct nv_ioctl_register_fd { int ctl_fd; } */
    { NV_ESC_REGISTER_FD,     {{0, FD_IN}},    1, -1 },
    /* struct nv_ioctl_alloc_os_event / free_os_event { NvHandle hClient, hDevice; NvU32 fd; ... } */
    { NV_ESC_ALLOC_OS_EVENT,  {{8, FD_IN}},    1, -1 },
    { NV_ESC_FREE_OS_EVENT,   {{8, FD_IN}},    1, -1 },
};

/*
 * Fds nested one level deeper, inside a parameter block that the ioctl argument points to (layouts
 * from NVIDIA's open kernel modules, tag 580.178.04: ctrl0000unix.h, cl0005.h, nvkms-api.h).
 */
#define NV_ESC_RM_CONTROL    _IOC(_IOC_READ | _IOC_WRITE, NV_IOCTL_MAGIC, 0x2a, 0x20)
#define NV_ESC_RM_ALLOC      _IOC(_IOC_READ | _IOC_WRITE, NV_IOCTL_MAGIC, 0x2b, 0x30)
#define NV01_EVENT_OS_EVENT  0x79
/* nvidia-modeset (same character major): struct NvKmsIoctlParams { NvU32 cmd, size; NvU64 address; } */
#define NVKMS_IOCTL_IOWR     _IOC(_IOC_READ | _IOC_WRITE, 'm', 0, 16)
#define NVKMS_IOCTL_REGISTER_SURFACE 17
#define NVKMS_IOCTL_GRANT_SURFACE    19
#define NVKMS_IOCTL_ACQUIRE_SURFACE  20
#define NVKMS_MAX_PLANES_PER_SURFACE 3

#define MAX_NESTED_FDS 3
struct nested_fds {
    size_t cnt;
    struct {
        void* ptr;
        bool is_64bit;
        int64_t saved;
    } f[MAX_NESTED_FDS];
};

/* Adds the fd field at `params + offset` if [params, params + size) is user memory. */
static void add_nested_fd(struct nested_fds* n, uint64_t params, uint64_t size, uint64_t offset,
                          bool is_64bit) {
    size_t width = is_64bit ? sizeof(int64_t) : sizeof(int32_t);
    if (n->cnt == MAX_NESTED_FDS || !params || offset + width > size
            || !is_user_memory_writable((void*)(uintptr_t)params, size))
        return;
    n->f[n->cnt].ptr = (void*)(uintptr_t)(params + offset);
    n->f[n->cnt].is_64bit = is_64bit;
    n->cnt++;
}

static void find_nvidia_nested_fds(unsigned int cmd, unsigned long arg, struct nested_fds* n) {
    if (cmd == NV_ESC_RM_CONTROL && is_user_memory_readable((void*)arg, 0x20)) {
        /* NVOS54_PARAMETERS { NvHandle hClient, hObject; NvU32 cmd, flags; NvP64 params;
         * NvU32 paramsSize; NvU32 status; }; the NV0000 OS_UNIX controls with an fd: */
        uint32_t rm_cmd = *(uint32_t*)(arg + 8);
        uint64_t params = *(uint64_t*)(arg + 16);
        uint32_t size = *(uint32_t*)(arg + 24);
        if (rm_cmd == 0x3d05) /* EXPORT_OBJECT_TO_FD { EXPORT_OBJECT object (16 B); NvS32 fd; } */
            add_nested_fd(n, params, size, 16, false);
        else if (rm_cmd == 0x3d06 || rm_cmd == 0x3d08 || rm_cmd == 0x3d0b || rm_cmd == 0x3d0c)
            /* IMPORT_OBJECT_FROM_FD, GET_EXPORT_OBJECT_INFO, EXPORT_OBJECTS_TO_FD,
             * IMPORT_OBJECTS_FROM_FD: { NvS32 fd; ... } */
            add_nested_fd(n, params, size, 0, false);
    } else if (cmd == NV_ESC_RM_ALLOC && is_user_memory_readable((void*)arg, 0x30)) {
        /* NVOS64_PARAMETERS { NvHandle hRoot, hObjectParent, hObjectNew; NvV32 hClass;
         * NvP64 pAllocParms; ... }; for NV01_EVENT_OS_EVENT the parameters are
         * NV0005_ALLOC_PARAMETERS { NvHandle hParentClient, hSrcResource; NvV32 hClass,
         * notifyIndex; NvP64 data; } and `data` is the fd registered with NV_ESC_ALLOC_OS_EVENT. */
        if (*(uint32_t*)(arg + 12) == NV01_EVENT_OS_EVENT)
            add_nested_fd(n, *(uint64_t*)(arg + 16), 24, 16, true);
    } else if (cmd == NVKMS_IOCTL_IOWR && is_user_memory_readable((void*)arg, 16)) {
        uint32_t kms_cmd = *(uint32_t*)arg;
        uint32_t size = *(uint32_t*)(arg + 4);
        uint64_t params = *(uint64_t*)(arg + 8);
        if (kms_cmd == NVKMS_IOCTL_REGISTER_SURFACE) {
            /* NvKmsRegisterSurfaceRequest { NvKmsDeviceHandle deviceHandle; NvBool useFd;
             * NvU32 rmClient; struct { union { NvU32 rmObject; NvS32 fd; } u; NvU64 offset;
             * NvU32 pitch; NvU64 rmObjectSizeInBytes; } planes[3]; ... }: planes at 16, 32 B
             * each; a plane is used when its size is not 0 */
            if (!params || size < 16 + 32 * NVKMS_MAX_PLANES_PER_SURFACE
                    || !is_user_memory_readable((void*)(uintptr_t)params, size)
                    || !*(uint8_t*)(uintptr_t)(params + 4))
                return;
            for (size_t i = 0; i < NVKMS_MAX_PLANES_PER_SURFACE; i++)
                if (*(uint64_t*)(uintptr_t)(params + 16 + 32 * i + 24))
                    add_nested_fd(n, params, size, 16 + 32 * i, false);
        } else if (kms_cmd == NVKMS_IOCTL_GRANT_SURFACE) {
            /* NvKmsGrantSurfaceRequest { deviceHandle; surfaceHandle; int fd; } */
            add_nested_fd(n, params, size, 8, false);
        } else if (kms_cmd == NVKMS_IOCTL_ACQUIRE_SURFACE) {
            /* NvKmsAcquireSurfaceRequest { int fd; } */
            add_nested_fd(n, params, size, 0, false);
        }
    }
}

static bool is_nvidia_device(struct libos_handle* hdl) {
    if (hdl->type != TYPE_CHROOT || !hdl->dentry)
        return false;
    lock(&g_dcache_lock);
    bool ret = hdl->dentry->inode && hdl->dentry->inode->type == S_IFCHR
               && hdl->dentry->inode->rdev_major == NV_MAJOR_DEVICE_NUMBER;
    unlock(&g_dcache_lock);
    return ret;
}

static bool raw_device_ioctl_enabled(void) {
    static int enabled = -1;
    int val = __atomic_load_n(&enabled, __ATOMIC_RELAXED);
    if (val < 0) {
        bool on = false;
        if (toml_bool_in(g_manifest_root, "sys.insecure__allow_raw_device_ioctl",
                         /*defaultval=*/false, &on) < 0)
            on = false;
        val = on;
        __atomic_store_n(&enabled, val, __ATOMIC_RELAXED);
    }
    return val;
}

int hostfd_install(PAL_HANDLE pal_handle, bool cloexec) {
    struct libos_handle* hdl = get_new_handle();
    if (!hdl)
        return -ENOMEM;

    PAL_STREAM_ATTR attr;
    memset(&attr, 0, sizeof(attr));
    int ret = PalStreamAttributesQueryByHandle(pal_handle, &attr);
    if (ret < 0) {
        put_handle(hdl);
        return pal_to_unix_errno(ret);
    }

    hdl->type = TYPE_HOSTFD;
    hdl->fs = &hostfd_builtin_fs;
    hdl->pal_handle = pal_handle;
    hdl->flags = O_RDWR | (attr.nonblocking ? O_NONBLOCK : 0);
    hdl->acc_mode = MAY_READ | MAY_WRITE;

    ret = set_new_fd_handle(hdl, cloexec ? FD_CLOEXEC : 0, NULL);
    /* on failure, `put_handle()` destroys the PAL handle (and closes the host fd) */
    put_handle(hdl);
    return ret;
}

/* The host fd behind a LibOS fd, or -1 (the host then reports EBADF). */
static int host_fd_of(int fd) {
    struct libos_handle* hdl = get_fd_handle(fd, NULL, NULL);
    if (!hdl)
        return -1;
    int host_fd = -1;
    if (hdl->pal_handle && PalHostFdGet(hdl->pal_handle, &host_fd) < 0)
        host_fd = -1;
    put_handle(hdl);
    return host_fd;
}

/* An absolute deadline on Saguaro's clock, moved to the host's CLOCK_MONOTONIC. 0 (no wait) and
 * INT64_MAX (no deadline) keep their meaning. */
static int64_t host_deadline(int64_t deadline) {
    if (deadline <= 0 || deadline == INT64_MAX)
        return deadline;
    uint64_t pal_us = 0, host_ns = 0;
    if (PalSystemTimeQuery(&pal_us) < 0 || PalHostClockMonotonic(&host_ns) < 0)
        return deadline;
    int64_t remaining = deadline - (int64_t)(pal_us * 1000);
    int64_t moved;
    if (__builtin_add_overflow((int64_t)host_ns, remaining, &moved))
        return remaining > 0 ? INT64_MAX : 0;
    return moved > 0 ? moved : 0;
}

int raw_device_ioctl(struct libos_handle* hdl, unsigned int cmd, unsigned long arg) {
    const struct translated_ioctl* ti = NULL;
    struct nested_fds nested = { .cnt = 0 };
    if (raw_device_ioctl_enabled()) {
        for (size_t i = 0; i < ARRAY_SIZE(g_translated_ioctls); i++) {
            if (g_translated_ioctls[i].cmd == cmd) {
                ti = &g_translated_ioctls[i];
                break;
            }
        }
        if (!ti && (_IOC_TYPE(cmd) == NV_IOCTL_MAGIC || cmd == NVKMS_IOCTL_IOWR)
                && is_nvidia_device(hdl)) {
            for (size_t i = 0; i < ARRAY_SIZE(g_translated_nvidia_ioctls); i++) {
                if (g_translated_nvidia_ioctls[i].cmd == cmd) {
                    ti = &g_translated_nvidia_ioctls[i];
                    break;
                }
            }
            find_nvidia_nested_fds(cmd, arg, &nested);
        }
    }

    /* replaced by the host fds for the call, restored afterwards */
    for (size_t i = 0; i < nested.cnt; i++) {
        if (nested.f[i].is_64bit) {
            nested.f[i].saved = *(int64_t*)nested.f[i].ptr;
            *(int64_t*)nested.f[i].ptr = host_fd_of((int32_t)nested.f[i].saved);
        } else {
            nested.f[i].saved = *(int32_t*)nested.f[i].ptr;
            *(int32_t*)nested.f[i].ptr = host_fd_of((int32_t)nested.f[i].saved);
        }
    }

    int64_t saved[2] = {0};
    if (ti) {
        if (!is_user_memory_writable((void*)arg, _IOC_SIZE(cmd)))
            return -EFAULT;
        for (size_t i = 0; i < ti->fields_cnt; i++) {
            void* field = (void*)(arg + ti->fields[i].offset);
            switch (ti->fields[i].kind) {
                case FD_IN:
                    saved[i] = *(int32_t*)field;
                    *(int32_t*)field = host_fd_of((int32_t)saved[i]);
                    break;
                case TIME_ABS:
                    saved[i] = *(int64_t*)field;
                    *(int64_t*)field = host_deadline(saved[i]);
                    break;
                case FD_OUT:
                    break;
            }
        }
    }

    int cmd_ret;
    int ret = PalDeviceIoControl(hdl->pal_handle, cmd, arg, &cmd_ret);
    if (ret < 0)
        ret = pal_to_unix_errno(ret);
    else
        ret = cmd_ret;

    for (size_t i = 0; i < nested.cnt; i++) {
        if (nested.f[i].is_64bit)
            *(int64_t*)nested.f[i].ptr = nested.f[i].saved;
        else
            *(int32_t*)nested.f[i].ptr = (int32_t)nested.f[i].saved;
    }

    if (ti) {
        for (size_t i = 0; i < ti->fields_cnt; i++) {
            void* field = (void*)(arg + ti->fields[i].offset);
            if (ti->fields[i].kind == FD_IN) {
                *(int32_t*)field = (int32_t)saved[i];
                continue;
            }
            if (ti->fields[i].kind == TIME_ABS) {
                *(int64_t*)field = saved[i];
                continue;
            }
            if (ret < 0)
                continue;
            int host_fd = *(int32_t*)field;
            PAL_HANDLE pal_handle = NULL;
            int wrap_ret = PalHostFdWrap(host_fd, &pal_handle);
            if (wrap_ret < 0) {
                log_warning("raw ioctl 0x%x: cannot wrap host fd %d: %s", cmd, host_fd,
                            pal_strerror(wrap_ret));
                *(int32_t*)field = -1;
                ret = pal_to_unix_errno(wrap_ret);
                continue;
            }
            bool cloexec = true;
            if (ti->cloexec_flags_offset >= 0)
                cloexec = *(uint32_t*)(arg + ti->cloexec_flags_offset) & DRM_CLOEXEC;
            int fd = hostfd_install(pal_handle, cloexec);
            if (fd < 0) {
                *(int32_t*)field = -1;
                ret = fd;
                continue;
            }
            *(int32_t*)field = fd;
        }
    }
    return ret;
}

static ssize_t hostfd_read(struct libos_handle* hdl, void* buf, size_t count, file_off_t* pos) {
    __UNUSED(pos);
    int ret = PalStreamRead(hdl->pal_handle, /*offset=*/0, &count, buf);
    if (ret < 0)
        return pal_to_unix_errno(ret);
    return (ssize_t)count;
}

static ssize_t hostfd_write(struct libos_handle* hdl, const void* buf, size_t count,
                            file_off_t* pos) {
    __UNUSED(pos);
    int ret = PalStreamWrite(hdl->pal_handle, /*offset=*/0, &count, (void*)buf);
    if (ret < 0)
        return pal_to_unix_errno(ret);
    return (ssize_t)count;
}

static int hostfd_mmap(struct libos_handle* hdl, void* addr, size_t size, int prot, int flags,
                       uint64_t offset) {
    if (flags & MAP_ANONYMOUS)
        return -EINVAL;
    int ret = PalStreamMap(hdl->pal_handle, addr, LINUX_PROT_TO_PAL(prot, flags), offset, size);
    return pal_to_unix_errno(ret);
}

static int hostfd_hstat(struct libos_handle* hdl, struct stat* buf) {
    PAL_STREAM_ATTR attr;
    memset(&attr, 0, sizeof(attr));
    int ret = PalStreamAttributesQueryByHandle(hdl->pal_handle, &attr);
    if (ret < 0)
        return pal_to_unix_errno(ret);

    memset(buf, 0, sizeof(*buf));
    buf->st_mode = attr.dev.host_mode ? attr.dev.host_mode : (S_IFREG | attr.share_flags);
    buf->st_size = attr.pending_size;
    buf->st_rdev = libos_makedev(attr.dev.rdev_major, attr.dev.rdev_minor);
    buf->st_nlink = 1;
    buf->st_blksize = 0x1000;
    return 0;
}

/* A dma-buf answers lseek(SEEK_END) with its size, and SEEK_SET/SEEK_CUR to 0 only; the position
 * is kept here, the host fd's own offset is not used by read/write of these handles. */
static file_off_t hostfd_seek(struct libos_handle* hdl, file_off_t offset, int whence) {
    file_off_t base;
    switch (whence) {
        case SEEK_SET:
            base = 0;
            break;
        case SEEK_CUR:
            base = hdl->pos;
            break;
        case SEEK_END: {
            struct stat st;
            int ret = hostfd_hstat(hdl, &st);
            if (ret < 0)
                return ret;
            base = st.st_size;
            break;
        }
        default:
            return -EINVAL;
    }
    file_off_t pos;
    if (__builtin_add_overflow(base, offset, &pos) || pos < 0)
        return -EINVAL;
    hdl->pos = pos;
    return pos;
}

static int hostfd_ioctl(struct libos_handle* hdl, unsigned int cmd, unsigned long arg) {
    return raw_device_ioctl(hdl, cmd, arg);
}

static struct libos_fs_ops hostfd_fs_ops = {
    .read  = &hostfd_read,
    .write = &hostfd_write,
    .mmap  = &hostfd_mmap,
    .hstat = &hostfd_hstat,
    .seek  = &hostfd_seek,
    .ioctl = &hostfd_ioctl,
};

struct libos_fs hostfd_builtin_fs = {
    .name   = "hostfd",
    .fs_ops = &hostfd_fs_ops,
};
