/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/*
 * Host file descriptors without a path, and raw device ioctls (gramine-direct only).
 *
 * With `sys.insecure__allow_raw_device_ioctl`, the Linux PAL passes device ioctls to the host with
 * the application's argument (see `_PalDeviceIoControl()`). Some ioctls carry file descriptors
 * inside their argument: DRM PRIME and syncobj export/import, dma-buf sync files. Those are host
 * fds on the host side and LibOS fds on the application side, so this file translates them:
 * an input LibOS fd is replaced by the host fd of its handle for the duration of the call, and an
 * output host fd is wrapped into a new `TYPE_HOSTFD` handle whose LibOS fd is written back.
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

/* The ioctls below, from the kernel's uapi headers (drm/drm.h, linux/dma-buf.h, linux/sync_file.h);
 * the sizes are those of the argument structures. */
#define DRM_IOCTL_PRIME_HANDLE_TO_FD    _IOC(_IOC_READ | _IOC_WRITE, 'd', 0x2d, 12)
#define DRM_IOCTL_PRIME_FD_TO_HANDLE    _IOC(_IOC_READ | _IOC_WRITE, 'd', 0x2e, 12)
#define DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD  _IOC(_IOC_READ | _IOC_WRITE, 'd', 0xc1, 24)
#define DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE  _IOC(_IOC_READ | _IOC_WRITE, 'd', 0xc2, 24)
#define DMA_BUF_IOCTL_EXPORT_SYNC_FILE  _IOC(_IOC_READ | _IOC_WRITE, 'b', 2, 8)
#define DMA_BUF_IOCTL_IMPORT_SYNC_FILE  _IOC(_IOC_WRITE, 'b', 3, 8)
#define SYNC_IOC_MERGE                  _IOC(_IOC_READ | _IOC_WRITE, '>', 3, 48)

/* The O_CLOEXEC flag of struct drm_prime_handle (DRM_CLOEXEC) */
#define DRM_CLOEXEC 02000000

struct fd_field {
    uint16_t offset;  /* of the `int32_t` fd in the argument */
    bool out;         /* written by the kernel (else read by it) */
};

struct fd_ioctl {
    unsigned int cmd;
    struct fd_field fields[2];
    size_t fields_cnt;
    int16_t cloexec_flags_offset; /* offset of a flags word with DRM_CLOEXEC, or -1: always cloexec */
};

static const struct fd_ioctl g_fd_ioctls[] = {
    /* struct drm_prime_handle { u32 handle; u32 flags; s32 fd; } */
    { DRM_IOCTL_PRIME_HANDLE_TO_FD,   {{8, true}},  1, 4 },
    { DRM_IOCTL_PRIME_FD_TO_HANDLE,   {{8, false}}, 1, -1 },
    /* struct drm_syncobj_handle { u32 handle; u32 flags; s32 fd; u32 pad; u64 point; } */
    { DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD, {{8, true}},  1, -1 },
    { DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE, {{8, false}}, 1, -1 },
    /* struct dma_buf_export_sync_file { u32 flags; s32 fd; } (and the import one) */
    { DMA_BUF_IOCTL_EXPORT_SYNC_FILE, {{4, true}},  1, -1 },
    { DMA_BUF_IOCTL_IMPORT_SYNC_FILE, {{4, false}}, 1, -1 },
    /* struct sync_merge_data { char name[32]; s32 fd2; s32 fence; u32 flags; u32 pad; } */
    { SYNC_IOC_MERGE,                 {{32, false}, {36, true}}, 2, -1 },
};

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

int raw_device_ioctl(struct libos_handle* hdl, unsigned int cmd, unsigned long arg) {
    const struct fd_ioctl* fdi = NULL;
    if (raw_device_ioctl_enabled()) {
        for (size_t i = 0; i < ARRAY_SIZE(g_fd_ioctls); i++) {
            if (g_fd_ioctls[i].cmd == cmd) {
                fdi = &g_fd_ioctls[i];
                break;
            }
        }
    }

    int32_t saved[2] = {0};
    if (fdi) {
        if (!is_user_memory_writable((void*)arg, _IOC_SIZE(cmd)))
            return -EFAULT;
        for (size_t i = 0; i < fdi->fields_cnt; i++) {
            int32_t* field = (int32_t*)(arg + fdi->fields[i].offset);
            saved[i] = *field;
            if (!fdi->fields[i].out)
                *field = host_fd_of(saved[i]);
        }
    }

    int cmd_ret;
    int ret = PalDeviceIoControl(hdl->pal_handle, cmd, arg, &cmd_ret);
    if (ret < 0)
        ret = pal_to_unix_errno(ret);
    else
        ret = cmd_ret;

    if (fdi) {
        for (size_t i = 0; i < fdi->fields_cnt; i++) {
            int32_t* field = (int32_t*)(arg + fdi->fields[i].offset);
            if (!fdi->fields[i].out) {
                *field = saved[i];
                continue;
            }
            if (ret < 0)
                continue;
            int host_fd = *field;
            PAL_HANDLE pal_handle = NULL;
            int wrap_ret = PalHostFdWrap(host_fd, &pal_handle);
            if (wrap_ret < 0) {
                log_warning("raw ioctl 0x%x: cannot wrap host fd %d: %s", cmd, host_fd,
                            pal_strerror(wrap_ret));
                *field = -1;
                ret = pal_to_unix_errno(wrap_ret);
                continue;
            }
            bool cloexec = true;
            if (fdi->cloexec_flags_offset >= 0)
                cloexec = *(uint32_t*)(arg + fdi->cloexec_flags_offset) & DRM_CLOEXEC;
            int fd = hostfd_install(pal_handle, cloexec);
            if (fd < 0) {
                *field = -1;
                ret = fd;
                continue;
            }
            *field = fd;
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
