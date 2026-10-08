/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * fcntl(F_DUPFD, F_DUPFD_CLOEXEC) follow the kernel "lowest free fd >= arg" (minimum)
 * semantics, including the self-referential minimum that tokio uses as an epoll probe
 * (`fcntl(epfd, F_DUPFD_CLOEXEC, epfd)` -- the exact placement returned EBADF when the
 * commands assigned exactly `arg`, see the F_DUPFD_CLOEXEC case in libos_fcntl.c).
 */

#define _GNU_SOURCE

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <unistd.h>

#define PATH "fcntl_dupfd.tmp"

static int open_tmpfile(void) {
    int fd = open(PATH, O_CREAT | O_TRUNC | O_RDWR, 0600);
    if (fd < 0) {
        perror("open");
        exit(1);
    }
    return fd;
}

int main(void) {
    int fd = open_tmpfile();
    assert(write(fd, "abc", 3) == 3);

    /* minimum below/equal the occupied source fd: must pick a free one, not fail */
    int d1 = fcntl(fd, F_DUPFD_CLOEXEC, fd);
    if (d1 < 0 || d1 <= fd) {
        fprintf(stderr, "F_DUPFD_CLOEXEC(min=%d) -> %d (errno=%d)\n", fd, d1, errno);
        return 1;
    }
    if (!(fcntl(d1, F_GETFD) & FD_CLOEXEC)) {
        puts("F_DUPFD_CLOEXEC: CLOEXEC flag not set");
        return 1;
    }

    /* a free high minimum is honored exactly */
    int d2 = fcntl(fd, F_DUPFD_CLOEXEC, 100);
    assert(d2 == 100);

    /* F_DUPFD with the same self-referential minimum */
    int d3 = fcntl(fd, F_DUPFD, fd);
    assert(d3 > fd);

    /* the duplicate shares the file position */
    char buf[4] = {0};
    assert(pread(d1, buf, 3, 0) == 3 && !memcmp(buf, "abc", 3));

    /* the exact shape that tokio's runtime build uses to probe epoll support */
    int ep = epoll_create1(EPOLL_CLOEXEC);
    int ep2 = fcntl(ep, F_DUPFD_CLOEXEC, ep);
    if (ep2 < 0 || ep2 <= ep) {
        fprintf(stderr, "epoll probe fcntl(%d, F_DUPFD_CLOEXEC, %d) -> %d (errno=%d)\n", ep, ep,
                ep2, errno);
        return 1;
    }

    close(ep2);
    close(ep);
    close(d1);
    close(d2);
    close(d3);
    close(fd);
    unlink(PATH);
    puts("TEST OK");
    return 0;
}
