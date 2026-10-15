/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/*
 * Error answers of the socket calls in the kernel's order (LTP accept01, listen01, socket01,
 * socketpair01, setsockopt01, bind02): accept() and listen() on a datagram socket are EOPNOTSUPP,
 * accept() on a socket that does not listen is EINVAL before its address buffer is looked at,
 * unknown socket types are EINVAL and protocols that do not fit the type EPROTONOSUPPORT, inet
 * socketpair() is EOPNOTSUPP after those checks, a socket-level option shorter than an int is
 * EINVAL, and binding a port below 1024 without CAP_NET_BIND_SERVICE is EACCES.
 */

#define _GNU_SOURCE

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

#define CHECK(cond)                                                \
    do {                                                           \
        if (!(cond)) {                                             \
            fprintf(stderr, "%s:%d: %s failed (errno=%d)\n",       \
                    __FILE__, __LINE__, #cond, errno);             \
            exit(1);                                               \
        }                                                          \
    } while (0)

int main(void) {
    struct sockaddr_in sin = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    socklen_t len = sizeof(sin);

    int udp = socket(AF_INET, SOCK_DGRAM, 0);
    int tcp = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(udp >= 0 && tcp >= 0);
    CHECK(accept(udp, (struct sockaddr*)&sin, &len) == -1 && errno == EOPNOTSUPP);
    CHECK(listen(udp, 1) == -1 && errno == EOPNOTSUPP);
    CHECK(accept(tcp, (struct sockaddr*)3, &len) == -1 && errno == EINVAL); /* not listening */
    socklen_t small = 1;
    CHECK(accept(tcp, (struct sockaddr*)&sin, &small) == -1 && errno == EINVAL);

    CHECK(socket(AF_INET, 75, 0) == -1 && errno == EINVAL);
    CHECK(socket(AF_INET, SOCK_STREAM, IPPROTO_UDP) == -1 && errno == EPROTONOSUPPORT);
    CHECK(socket(AF_INET, SOCK_DGRAM, IPPROTO_TCP) == -1 && errno == EPROTONOSUPPORT);
    int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    CHECK(s >= 0);
    close(s);

    int sv[2];
    CHECK(socketpair(0, SOCK_STREAM, 0, sv) == -1 && errno == EAFNOSUPPORT);
    CHECK(socketpair(AF_INET, 75, 0, sv) == -1 && errno == EINVAL);
    CHECK(socketpair(AF_INET, SOCK_DGRAM, IPPROTO_UDP, sv) == -1 && errno == EOPNOTSUPP);
    CHECK(socketpair(AF_INET, SOCK_DGRAM, IPPROTO_TCP, sv) == -1 && errno == EPROTONOSUPPORT);
    CHECK(socketpair(AF_INET, SOCK_STREAM, IPPROTO_TCP, sv) == -1 && errno == EOPNOTSUPP);

    int one = 1;
    CHECK(setsockopt(tcp, SOL_SOCKET, SO_KEEPALIVE, &one, 0) == -1 && errno == EINVAL);
    CHECK(setsockopt(tcp, SOL_SOCKET, SO_KEEPALIVE, NULL, sizeof(one)) == -1 && errno == EFAULT);

    /* privileged ports, as an unprivileged user (no real bind happens) */
    CHECK(seteuid(65534) == 0);
    int t2 = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(t2 >= 0);
    struct sockaddr_in low = {.sin_family = AF_INET, .sin_port = htons(80),
                              .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    CHECK(bind(t2, (struct sockaddr*)&low, sizeof(low)) == -1 && errno == EACCES);
    close(t2);
    CHECK(seteuid(0) == 0);

    close(udp);
    close(tcp);
    puts("TEST OK");
    return 0;
}
