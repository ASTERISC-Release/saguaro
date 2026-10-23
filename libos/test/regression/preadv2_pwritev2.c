/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/*
 * Regression test for preadv2/pwritev2: explicit offsets, offset -1 (the file position), the
 * RWF_* flags on a regular file and the EOPNOTSUPP/EINVAL checks.
 *
 * The calls are raw syscalls with the kernel's argument layout, (fd, vec, vlen, pos_l, pos_h,
 * flags) on every architecture, and the offset split as libc splits it (pos_h = offset >> 32).
 * Until 2026-10 the LibOS read the flags from pos_h on aarch64 and riscv64, so offset -1 failed
 * with EOPNOTSUPP and real flags were ignored (dev docs: syscall-compat/TODO.md).
 *
 * Usage: preadv2_pwritev2 <base-dir>
 */

#define _GNU_SOURCE

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <unistd.h>

/* inlined so that the test does not depend on new kernel headers */
#define T_RWF_HIPRI  0x00000001
#define T_RWF_DSYNC  0x00000002
#define T_RWF_SYNC   0x00000004
#define T_RWF_NOWAIT 0x00000008
#define T_RWF_APPEND 0x00000010
#define T_RWF_BAD    0x80000000U

static long raw_rw2(bool write, int fd, void* buf, size_t len, off_t offset, unsigned int flags) {
    struct iovec iov = { .iov_base = buf, .iov_len = len };
    unsigned long pos_l = (unsigned long)offset;
    unsigned long pos_h = (unsigned long)((uint64_t)offset >> 32);
    return syscall(write ? SYS_pwritev2 : SYS_preadv2, fd, &iov, 1UL, pos_l, pos_h, flags);
}

static off_t cur_pos(int fd) {
    off_t pos = lseek(fd, 0, SEEK_CUR);
    if (pos < 0)
        err(1, "lseek");
    return pos;
}

static void expect_ret(const char* what, long ret, long expected) {
    if (ret != expected)
        errx(1, "%s: returned %ld (errno %d), expected %ld", what, ret, errno, expected);
}

static void expect_err(const char* what, long ret, int expected_errno) {
    if (ret != -1 || errno != expected_errno)
        errx(1, "%s: returned %ld (errno %d), expected -1 with errno %d", what, ret, errno,
             expected_errno);
}

static void expect_data(const char* what, const char* buf, const char* expected) {
    if (memcmp(buf, expected, strlen(expected)))
        errx(1, "%s: read \"%.*s\", expected \"%s\"", what, (int)strlen(expected), buf, expected);
}

int main(int argc, char** argv) {
    if (argc != 2)
        errx(1, "usage: %s <base-dir>", argv[0]);

    char path[512];
    if ((size_t)snprintf(path, sizeof(path), "%s/preadv2_pwritev2_file", argv[1]) >= sizeof(path))
        errx(1, "path too long");
    unlink(path);
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        err(1, "open %s", path);

    char buf[64];

    /* explicit offset, flags 0: the file position does not move */
    char data[] = "0123456789abcdef";
    expect_ret("pwritev2(offset 0)", raw_rw2(true, fd, data, 16, 0, 0), 16);
    expect_ret("position after pwritev2(offset 0)", cur_pos(fd), 0);

    /* offset -1: write at the file position and advance it */
    if (lseek(fd, 4, SEEK_SET) != 4)
        err(1, "lseek");
    char xy[] = "XY";
    expect_ret("pwritev2(offset -1)", raw_rw2(true, fd, xy, 2, -1, 0), 2);
    expect_ret("position after pwritev2(offset -1)", cur_pos(fd), 6);

    memset(buf, 0, sizeof(buf));
    expect_ret("preadv2(offset 0)", raw_rw2(false, fd, buf, 16, 0, 0), 16);
    expect_data("preadv2(offset 0)", buf, "0123XY6789abcdef");
    expect_ret("position after preadv2(offset 0)", cur_pos(fd), 6);

    memset(buf, 0, sizeof(buf));
    expect_ret("preadv2(offset -1)", raw_rw2(false, fd, buf, 4, -1, 0), 4);
    expect_data("preadv2(offset -1)", buf, "6789");
    expect_ret("position after preadv2(offset -1)", cur_pos(fd), 10);

    /* an explicit offset above 4 GiB-1 keeps its high half (pos_l holds the whole offset);
     * reading past the end of the file returns 0 */
    expect_ret("preadv2(offset 5 GiB)", raw_rw2(false, fd, buf, 4, 5LL << 30, 0), 0);

    /* valid flags on a regular file */
    memset(buf, 0, sizeof(buf));
    expect_ret("preadv2(RWF_HIPRI)", raw_rw2(false, fd, buf, 4, 2, T_RWF_HIPRI), 4);
    expect_data("preadv2(RWF_HIPRI)", buf, "23XY");

    memset(buf, 0, sizeof(buf));
    long ret = raw_rw2(false, fd, buf, 4, 12, T_RWF_NOWAIT);
    if (ret == 4) {
        expect_data("preadv2(RWF_NOWAIT)", buf, "cdef");
    } else if (!(ret == -1 && (errno == EAGAIN || errno == EOPNOTSUPP))) {
        /* Linux may also refuse RWF_NOWAIT where the filesystem lacks it (EOPNOTSUPP on tmpfs) */
        errx(1, "preadv2(RWF_NOWAIT): returned %ld (errno %d), expected 4, EAGAIN or EOPNOTSUPP",
             ret, errno);
    }

    char s[] = "S";
    expect_ret("pwritev2(RWF_SYNC)", raw_rw2(true, fd, s, 1, 0, T_RWF_SYNC), 1);
    char d[] = "D";
    expect_ret("pwritev2(RWF_DSYNC, offset -1)", raw_rw2(true, fd, d, 1, -1, T_RWF_DSYNC), 1);
    expect_ret("position after pwritev2(RWF_DSYNC, offset -1)", cur_pos(fd), 11);

    /* RWF_APPEND writes at the end of the file whatever the offset */
    char z[] = "Z";
    expect_ret("pwritev2(RWF_APPEND)", raw_rw2(true, fd, z, 1, 0, T_RWF_APPEND), 1);

    memset(buf, 0, sizeof(buf));
    expect_ret("preadv2(whole file)", raw_rw2(false, fd, buf, sizeof(buf), 0, 0), 17);
    expect_data("preadv2(whole file)", buf, "S123XY6789DbcdefZ");

    /* an unknown flag gives EOPNOTSUPP, with an explicit offset and with offset -1 */
    expect_err("preadv2(bad flag)", raw_rw2(false, fd, buf, 4, 0, T_RWF_BAD), EOPNOTSUPP);
    expect_err("preadv2(bad flag, offset -1)", raw_rw2(false, fd, buf, 4, -1, T_RWF_BAD),
               EOPNOTSUPP);
    expect_err("pwritev2(bad flag)", raw_rw2(true, fd, buf, 4, 0, T_RWF_BAD), EOPNOTSUPP);

    /* a negative offset other than -1 gives EINVAL */
    expect_err("preadv2(offset -2)", raw_rw2(false, fd, buf, 4, -2, 0), EINVAL);
    expect_err("pwritev2(offset -2)", raw_rw2(true, fd, buf, 4, -2, 0), EINVAL);

    if (close(fd) < 0)
        err(1, "close");
    if (unlink(path) < 0)
        err(1, "unlink");

    puts("TEST OK");
    return 0;
}
