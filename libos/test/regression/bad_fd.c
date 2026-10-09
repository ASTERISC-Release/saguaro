/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/* Operations on invalid fds must fail with EBADF. Regression test for the Tirith "graphics"
 * (graphics) paths in close()/lseek()/getdents64() that keyed on the raw fd number and, without a host
 * agent (plain Linux PAL), faulted on the unmapped graphics comm window (iperf3 server crash). */

#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "common.h"

int main(void) {
    int fds[] = {7, 63, 400, 499, 500, 501, 700, 100000};
    for (size_t i = 0; i < sizeof(fds) / sizeof(fds[0]); i++) {
        int fd = fds[i];
        errno = 0;
        if (close(fd) != -1 || errno != EBADF)
            errx(1, "close(%d): expected EBADF, got errno %d", fd, errno);
        errno = 0;
        if (lseek(fd, 0, SEEK_SET) != -1 || errno != EBADF)
            errx(1, "lseek(%d): expected EBADF, got errno %d", fd, errno);
        char buf[256];
        errno = 0;
        if (syscall(SYS_getdents64, fd, buf, sizeof(buf)) != -1 || errno != EBADF)
            errx(1, "getdents64(%d): expected EBADF, got errno %d", fd, errno);
    }
    puts("TEST OK");
    return 0;
}
