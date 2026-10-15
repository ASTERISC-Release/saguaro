/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/*
 * poll() and ppoll() that time out must report revents == 0 for every fd, also when the caller
 * left garbage in revents (Linux writes revents in every case). The NVIDIA GLX library polls its X
 * connection with events = 0 and a zero timeout and checks revents.
 */

#define _GNU_SOURCE
#include <err.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define GARBAGE ((short)0x5a5a)

static void check(const char* what, int ret, struct pollfd* fds, size_t n) {
    if (ret != 0)
        errx(1, "%s: returned %d, expected 0 (timeout)", what, ret);
    for (size_t i = 0; i < n; i++)
        if (fds[i].revents != 0)
            errx(1, "%s: fd %d revents = 0x%x, expected 0", what, fds[i].fd,
                 (unsigned short)fds[i].revents);
}

int main(void) {
    int pipefds[2];
    int sv[2];
    if (pipe(pipefds) < 0)
        err(1, "pipe");
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0)
        err(1, "socketpair");

    /* events = 0, zero timeout */
    struct pollfd fds[2] = {
        { .fd = pipefds[0], .events = 0, .revents = GARBAGE },
        { .fd = sv[0], .events = 0, .revents = GARBAGE },
    };
    check("poll(events=0, 0 ms)", poll(fds, 2, 0), fds, 2);

    /* POLLIN on empty pipe and socket, zero and short timeouts */
    for (int timeout_ms = 0; timeout_ms <= 10; timeout_ms += 10) {
        fds[0] = (struct pollfd){ .fd = pipefds[0], .events = POLLIN, .revents = GARBAGE };
        fds[1] = (struct pollfd){ .fd = sv[0], .events = POLLIN, .revents = GARBAGE };
        check(timeout_ms ? "poll(POLLIN, 10 ms)" : "poll(POLLIN, 0 ms)",
              poll(fds, 2, timeout_ms), fds, 2);
    }

    struct timespec zero = { 0, 0 };
    fds[0] = (struct pollfd){ .fd = pipefds[0], .events = 0, .revents = GARBAGE };
    fds[1] = (struct pollfd){ .fd = sv[0], .events = POLLIN, .revents = GARBAGE };
    check("ppoll(0 s)", ppoll(fds, 2, &zero, NULL), fds, 2);

    puts("TEST OK");
    return 0;
}
