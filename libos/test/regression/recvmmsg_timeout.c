/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/*
 * recvmmsg() with a timeout (LTP sendmmsg01): the kernel checks the timeout only after each
 * received datagram, so with datagrams queued the call returns them all, and it writes the time
 * left back. Also checks scatter/gather contents of sendmmsg() over UDP loopback and the
 * validation of the timeout argument.
 */

#define _GNU_SOURCE

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

#define CHECK(cond)                                                \
    do {                                                           \
        if (!(cond)) {                                             \
            fprintf(stderr, "%s:%d: %s failed (errno=%d)\n",       \
                    __FILE__, __LINE__, #cond, errno);             \
            exit(1);                                               \
        }                                                          \
    } while (0)

static void run(bool probe_port) {
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    socklen_t alen = sizeof(addr);
    if (probe_port) {
        /* as LTP's TST_GET_UNUSED_PORT: bind a probe to port 0, note the port, close the probe */
        int probe = socket(AF_INET, SOCK_DGRAM, 0);
        CHECK(probe >= 0);
        CHECK(bind(probe, (struct sockaddr*)&addr, sizeof(addr)) == 0);
        CHECK(getsockname(probe, (struct sockaddr*)&addr, &alen) == 0);
        close(probe);
    }
    int rx = socket(AF_INET, SOCK_DGRAM, 0);
    int tx = socket(AF_INET, SOCK_DGRAM, 0);
    CHECK(rx >= 0 && tx >= 0);
    CHECK(bind(rx, (struct sockaddr*)&addr, sizeof(addr)) == 0);
    alen = sizeof(addr);
    CHECK(getsockname(rx, (struct sockaddr*)&addr, &alen) == 0);
    CHECK(connect(tx, (struct sockaddr*)&addr, sizeof(addr)) == 0);

    char one[] = "one", two[] = "two", three[] = "three3";
    struct iovec s1[2] = {{one, 3}, {two, 3}};
    struct iovec s2[1] = {{three, 6}};
    struct mmsghdr snd[2];
    memset(snd, 0, sizeof(snd));
    snd[0].msg_hdr.msg_iov = s1;
    snd[0].msg_hdr.msg_iovlen = 2;
    snd[1].msg_hdr.msg_iov = s2;
    snd[1].msg_hdr.msg_iovlen = 1;

    char r1[16], r2[16];
    struct iovec v1 = {r1, sizeof(r1)}, v2 = {r2, sizeof(r2)};
    struct mmsghdr rcv[2];

    for (int round = 0; round < 2; round++) {
        CHECK(sendmmsg(tx, snd, 2, 0) == 2);
        CHECK(snd[0].msg_len == 6 && snd[1].msg_len == 6);

        memset(r1, 0, sizeof(r1));
        memset(r2, 0, sizeof(r2));
        memset(rcv, 0, sizeof(rcv));
        rcv[0].msg_hdr.msg_iov = &v1;
        rcv[0].msg_hdr.msg_iovlen = 1;
        rcv[1].msg_hdr.msg_iov = &v2;
        rcv[1].msg_hdr.msg_iovlen = 1;
        struct timespec timeout = {.tv_sec = 1, .tv_nsec = 0};
        int n = recvmmsg(rx, rcv, 2, 0, &timeout);
        CHECK(n == 2);
        CHECK(rcv[0].msg_len == 6 && memcmp(r1, "onetwo", 6) == 0);
        CHECK(rcv[1].msg_len == 6 && memcmp(r2, "three3", 6) == 0);
        /* the time left is written back and never grows */
        CHECK(timeout.tv_sec >= 0 && timeout.tv_sec <= 1);
        CHECK(timeout.tv_sec == 0 || timeout.tv_nsec == 0);
    }

    /* an expired timeout still returns the first datagram, then stops */
    CHECK(sendmmsg(tx, snd, 2, 0) == 2);
    memset(rcv, 0, sizeof(rcv));
    rcv[0].msg_hdr.msg_iov = &v1;
    rcv[0].msg_hdr.msg_iovlen = 1;
    rcv[1].msg_hdr.msg_iov = &v2;
    rcv[1].msg_hdr.msg_iovlen = 1;
    struct timespec zero = {0, 0};
    CHECK(recvmmsg(rx, rcv, 2, 0, &zero) == 1);
    CHECK(recvmmsg(rx, rcv, 2, MSG_DONTWAIT, NULL) == 1); /* the second one is still queued */

    struct timespec bad = {.tv_sec = 0, .tv_nsec = 1000000000L};
    CHECK(recvmmsg(rx, rcv, 2, MSG_DONTWAIT, &bad) == -1 && errno == EINVAL);

    close(tx);
    close(rx);
}

int main(void) {
    /* twice, with fresh sockets and a fresh port (LTP reruns its setup per test variant) */
    run(false);
    run(true);
    run(true);
    puts("TEST OK");
    return 0;
}
