/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2021 Intel Corporation
 *                    Paweł Marczewski <pawel@invisiblethingslab.com>
 */

/*!
 * \file
 *
 * This file contains the implementation of `/dev` pseudo-filesystem.
 */

#include "libos_flags_conv.h"
#include "libos_fs_pseudo.h"
#include "linux_abi/errors.h"
#include "pal.h"
#include "libos_graphics.h"
static ssize_t dev_null_read(struct libos_handle* hdl, void* buf, size_t count) {
    __UNUSED(hdl);
    __UNUSED(buf);
    __UNUSED(count);
    return 0;
}

static ssize_t dev_null_write(struct libos_handle* hdl, const void* buf, size_t count) {
    __UNUSED(hdl);
    __UNUSED(buf);
    __UNUSED(count);
    return count;
}

static int64_t dev_null_seek(struct libos_handle* hdl, int64_t offset, int whence) {
    __UNUSED(hdl);
    __UNUSED(offset);
    __UNUSED(whence);
    return 0;
}

/* TODO: ftruncate() on /dev/null should fail, but open() with O_TRUNC should succeed */
static int dev_null_truncate(struct libos_handle* hdl, uint64_t size) {
    __UNUSED(hdl);
    __UNUSED(size);
    return 0;
}

static ssize_t dev_zero_read(struct libos_handle* hdl, void* buf, size_t count) {
    __UNUSED(hdl);
    memset(buf, 0, count);
    return count;
}

/* /dev/full: reads as /dev/zero, every write fails with ENOSPC (Linux's drivers/char/mem.c).
 * ALSA's null PCM plugin opens it for playback, which Wine's audio under gramine-vm needs with a
 * `type null` default device (dev docs: proton-vm/FINDINGS.md, 2026-10-16). */
static ssize_t dev_full_write(struct libos_handle* hdl, const void* buf, size_t count) {
    __UNUSED(hdl);
    __UNUSED(buf);
    __UNUSED(count);
    return -ENOSPC;
}

static ssize_t dev_random_read(struct libos_handle* hdl, void* buf, size_t count) {
    __UNUSED(hdl);
    int ret = PalRandomBitsRead(buf, count);

    if (ret < 0)
        return pal_to_unix_errno(ret);
    return count;
}

/* RNDGETENTCNT reports a full entropy pool: the PAL's random source never blocks (LTP ioctl07);
 * the requests that change the pool need CAP_SYS_ADMIN, as on Linux, and do nothing */
#define RNDGETENTCNT   0x80045200
#define RNDADDTOENTCNT 0x40045201
#define RNDGETPOOL     0x80085202
#define RNDADDENTROPY  0x40085203
#define RNDZAPENTCNT   0x5204
#define RNDCLEARPOOL   0x5206
#define RNDRESEEDCRNG  0x5207
static int dev_random_ioctl(struct libos_handle* hdl, unsigned int cmd, unsigned long arg) {
    __UNUSED(hdl);
    switch (cmd) {
        case RNDGETENTCNT:
            if (!is_user_memory_writable((void*)arg, sizeof(int)))
                return -EFAULT;
            *(int*)arg = 256; /* the pool size in bits */
            return 0;
        case RNDADDTOENTCNT:
        case RNDADDENTROPY:
        case RNDZAPENTCNT:
        case RNDCLEARPOOL:
        case RNDRESEEDCRNG:
            return libos_capable(21 /* CAP_SYS_ADMIN */) ? 0 : -EPERM;
        default:
            return -EINVAL; /* the random driver's answer to an unknown request */
    }
}

static int dev_tty_open(struct libos_handle* hdl, struct libos_dentry* dent, int flags) {
    __UNUSED(dent);

    char* uri = strdup(URI_PREFIX_CONSOLE);
    if (!uri)
        return -ENOMEM;

    PAL_HANDLE palhdl;
    int ret = PalStreamOpen(uri, LINUX_OPEN_FLAGS_TO_PAL_ACCESS(flags), PSEUDO_PERM_FILE_RW,
                            PAL_CREATE_NEVER, /*options=*/0, &palhdl);
    if (ret < 0) {
        free(uri);
        return pal_to_unix_errno(ret);
    }

    assert(hdl);
    hdl->uri = uri;
    hdl->pal_handle = palhdl;
    return 0;
}

#ifdef ENABLE_GRAPHICS
    static int dev_gpudev_open(struct libos_handle* hdl, struct libos_dentry* dent, int flags) {
        __UNUSED(dent);
        __UNUSED(flags);
        log_debug("\"/dev\" device open completed.");
        return 0;
    }
#endif

static ssize_t dev_tty_read(struct libos_handle* hdl, void* buf, size_t count) {
    size_t actual_count = count;
    int ret = PalStreamRead(hdl->pal_handle, /*offset=*/0, &actual_count, buf);
    if (ret < 0)
        return pal_to_unix_errno(ret);

    assert(actual_count <= count);
    return actual_count;
}

static ssize_t dev_tty_write(struct libos_handle* hdl, const void* buf, size_t count) {
    size_t actual_count = count;
    int ret = PalStreamWrite(hdl->pal_handle, /*offset=*/0, &actual_count, (void*)buf);
    if (ret < 0)
        return pal_to_unix_errno(ret);

    assert(actual_count <= count);
    return actual_count;
}

static int dev_tty_flush(struct libos_handle* hdl) {
    int ret = PalStreamFlush(hdl->pal_handle);
    return pal_to_unix_errno(ret);
}

/* this dummy function is required only to override the default behavior of pseudo_poll() -- this is
 * because we actually want to poll on the host tty/console; see also libos_poll.c */
static int dev_tty_poll(struct libos_handle* hdl, int in_events, int* out_events) {
    __UNUSED(hdl);
    __UNUSED(in_events);
    __UNUSED(out_events);
    return -ENOSYS;
}

int init_devfs(void) {
    struct pseudo_node* root = pseudo_add_root_dir("dev");

    /* Device minor numbers for pseudo-devices:
     * https://elixir.bootlin.com/linux/v5.9/source/drivers/char/mem.c#L950 */

    struct pseudo_node* null = pseudo_add_dev(root, "null");
    null->perm = PSEUDO_PERM_FILE_RW;
    null->dev.major = 1;
    null->dev.minor = 3;
    null->dev.dev_ops.read = &dev_null_read;
    null->dev.dev_ops.write = &dev_null_write;
    null->dev.dev_ops.seek = &dev_null_seek;
    null->dev.dev_ops.truncate = &dev_null_truncate;

    struct pseudo_node* zero = pseudo_add_dev(root, "zero");
    zero->perm = PSEUDO_PERM_FILE_RW;
    zero->dev.major = 1;
    zero->dev.minor = 5;
    zero->dev.dev_ops.read = &dev_zero_read;
    zero->dev.dev_ops.write = &dev_null_write;
    zero->dev.dev_ops.seek = &dev_null_seek;
    zero->dev.dev_ops.truncate = &dev_null_truncate;

    struct pseudo_node* full = pseudo_add_dev(root, "full");
    full->perm = PSEUDO_PERM_FILE_RW;
    full->dev.major = 1;
    full->dev.minor = 7;
    full->dev.dev_ops.read = &dev_zero_read;
    full->dev.dev_ops.write = &dev_full_write;
    full->dev.dev_ops.seek = &dev_null_seek;
    full->dev.dev_ops.truncate = &dev_null_truncate;

    struct pseudo_node* random = pseudo_add_dev(root, "random");
    random->perm = PSEUDO_PERM_FILE_RW;
    random->dev.major = 1;
    random->dev.minor = 8;
    random->dev.dev_ops.read = &dev_random_read;
    /* writes in /dev/random add entropy in normal Linux, but not implemented in Gramine */
    random->dev.dev_ops.write = &dev_null_write;
    random->dev.dev_ops.seek = &dev_null_seek;
    random->dev.dev_ops.ioctl = &dev_random_ioctl;

    struct pseudo_node* urandom = pseudo_add_dev(root, "urandom");
    urandom->perm = PSEUDO_PERM_FILE_RW;
    urandom->dev.major = 1;
    urandom->dev.minor = 9;
    /* /dev/urandom is implemented the same as /dev/random, so it has the same operations */
    urandom->dev.dev_ops = random->dev.dev_ops;

    /* see `man 4 tty` for more info, including major/minor numbers */
    struct pseudo_node* tty = pseudo_add_dev(root, "tty");
    tty->perm = PSEUDO_PERM_FILE_RW;
    tty->dev.major = 5;
    tty->dev.minor = 0;
    tty->dev.dev_ops.open = &dev_tty_open;
    tty->dev.dev_ops.read = &dev_tty_read;
    tty->dev.dev_ops.write = &dev_tty_write;
    tty->dev.dev_ops.flush = &dev_tty_flush;
    tty->dev.dev_ops.poll = &dev_tty_poll;
    
    #ifdef ENABLE_GRAPHICS
    struct pseudo_node* driNode = pseudo_add_dir(/*parent_node=*/root, "dri");
    struct pseudo_node* gpudev = pseudo_add_dev(driNode, "renderD128");
    gpudev->perm = PSEUDO_PERM_FILE_RW;
    gpudev->dev.major = 226; // Official kernel mandates for /dri major #
    gpudev->dev.minor = 128;
    gpudev->dev.dev_ops.open = &dev_gpudev_open;

    struct pseudo_node* discrete_gpudev = pseudo_add_dev(driNode, "renderD129");
    discrete_gpudev->perm = PSEUDO_PERM_FILE_RW;
    discrete_gpudev->dev.major = 226; // Official kernel mandates for /dri major #
    discrete_gpudev->dev.minor = 128;
    discrete_gpudev->dev.dev_ops.open = &dev_gpudev_open;
    #endif
    
    struct pseudo_node* fd_link = pseudo_add_link(root, "fd", NULL);
    fd_link->link.target = "/proc/self/fd";

    struct pseudo_node* stdin = pseudo_add_link(root, "stdin", NULL);
    stdin->link.target = "/proc/self/fd/0";
    struct pseudo_node* stdout = pseudo_add_link(root, "stdout", NULL);
    stdout->link.target = "/proc/self/fd/1";
    struct pseudo_node* stderr = pseudo_add_link(root, "stderr", NULL);
    stderr->link.target = "/proc/self/fd/2";

    int ret = init_attestation(root);
    if (ret < 0)
        return ret;

    return 0;
}
