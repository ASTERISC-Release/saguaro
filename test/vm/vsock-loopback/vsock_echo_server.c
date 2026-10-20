/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/*
 * vsock_echo_server -- regression driver for host-initiated connections to a guest AF_INET TCP
 * server under the VM PAL (Documentation/devel/CHANGELOG.md, phase 1 of the plan).
 *
 * The host side (vsock_host_client.py) opens connections from the container into this server and
 * checks that they work repeatedly and that the listener survives them. Protocol per connection:
 *
 *   client: u32 sequence number (little endian), then `size` pseudorandom bytes; then half-close
 *   server: reads to EOF, writes everything back, closes
 *   client: checks the echo, closes
 *
 * A connection whose whole payload is the 7 bytes "GOODBYE" (and nothing else) makes the server
 * print "DONE <count>" and exit with code 0.
 *
 * Single-threaded on purpose: while the server serves one connection, the others must sit in the
 * listener's backlog -- that is exactly the accept bookkeeping this test exercises.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <sys/epoll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int read_all(int fd, unsigned char* buf, size_t cap, size_t* out_len) {
    size_t len = 0;
    while (len < cap) {
        ssize_t n = read(fd, buf + len, cap - len);
        if (n == 0) {
            break; /* the client half-closed: end of its request */
        }
        if (n < 0) {
            if (errno == EINTR)
                continue;
            perror("read");
            return -1;
        }
        len += (size_t)n;
    }
    *out_len = len;
    return 0;
}

static int write_all(int fd, const unsigned char* buf, size_t len) {
    while (len) {
        ssize_t n = write(fd, buf, len);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            perror("write");
            return -1;
        }
        buf += n;
        len -= (size_t)n;
    }
    return 0;
}

/* Persistent-connection regression for readiness wakeups: one EPOLLET event and one small echo
 * per synchronous request, so the client cannot hide a missed edge by pipelining more data. */
static int serve_epoll(const char* port, unsigned long requests) {
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) {
        perror("socket");
        return 1;
    }
    int one = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)atoi(port));
    if (bind(srv, (struct sockaddr*)&addr, sizeof(addr)) < 0 || listen(srv, 32) < 0) {
        perror("bind/listen");
        close(srv);
        return 1;
    }

    int epfd = epoll_create1(0);
    if (epfd < 0) {
        perror("epoll_create1");
        close(srv);
        return 1;
    }
    struct epoll_event ev = {
        .events = EPOLLIN | EPOLLET | EPOLLONESHOT,
        .data.fd = srv,
    };
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, srv, &ev) < 0) {
        perror("epoll_ctl listener");
        close(epfd);
        close(srv);
        return 1;
    }
    printf("LISTENING %s (epoll, %lu requests)\n", port, requests);
    fflush(stdout);

    int cli = -1;
    unsigned long handled = 0;
    for (;;) {
        struct epoll_event events[4];
        int n = epoll_wait(epfd, events, 4, -1);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            perror("epoll_wait");
            return 1;
        }
        for (int i = 0; i < n; i++) {
            if (events[i].data.fd == srv) {
                cli = accept(srv, NULL, NULL);
                if (cli < 0) {
                    perror("accept");
                    return 1;
                }
                ev.data.fd = cli;
                if (epoll_ctl(epfd, EPOLL_CTL_ADD, cli, &ev) < 0) {
                    perror("epoll_ctl client");
                    return 1;
                }
                continue;
            }

            unsigned char buf[4096];
            size_t got = 0;
            while (got < 64) {
                ssize_t part = read(events[i].data.fd, buf + got, sizeof(buf) - got);
                if (part < 0 && errno == EINTR)
                    continue;
                if (part <= 0) {
                    fprintf(stderr, "epoll echo: client closed after %lu requests\n", handled);
                    return 1;
                }
                got += (size_t)part;
            }
            if (write_all(events[i].data.fd, buf, got) < 0)
                return 1;
            handled++;
            if (handled % 100 == 0) {
                printf("EPOLL_HANDLED %lu\n", handled);
                fflush(stdout);
            }
            if (handled == requests) {
                printf("DONE %lu\n", handled);
                fflush(stdout);
                close(cli);
                close(epfd);
                close(srv);
                return 0;
            }
            /* libevent-style one-shot rearming after each callback. */
            ev.data.fd = events[i].data.fd;
            if (epoll_ctl(epfd, EPOLL_CTL_MOD, events[i].data.fd, &ev) < 0) {
                perror("epoll_ctl rearm");
                return 1;
            }
        }
    }
}

static int serve_blocking(const char* port, unsigned long requests) {
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) {
        perror("socket");
        return 1;
    }
    int one = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)atoi(port));
    if (bind(srv, (struct sockaddr*)&addr, sizeof(addr)) < 0 || listen(srv, 32) < 0) {
        perror("bind/listen");
        close(srv);
        return 1;
    }
    printf("LISTENING %s (blocking, %lu requests)\n", port, requests);
    fflush(stdout);

    int cli = accept(srv, NULL, NULL);
    if (cli < 0) {
        perror("accept");
        return 1;
    }
    unsigned long handled = 0;
    while (handled < requests) {
        unsigned char buf[64];
        size_t got = 0;
        while (got < sizeof(buf)) {
            ssize_t part = read(cli, buf + got, sizeof(buf) - got);
            if (part < 0 && errno == EINTR)
                continue;
            if (part <= 0) {
                fprintf(stderr, "blocking echo: client closed after %lu requests\n", handled);
                return 1;
            }
            got += (size_t)part;
        }
        if (write_all(cli, buf, sizeof(buf)) < 0)
            return 1;
        handled++;
        if (handled % 100 == 0) {
            printf("BLOCKING_HANDLED %lu\n", handled);
            fflush(stdout);
        }
    }
    printf("DONE %lu\n", handled);
    fflush(stdout);
    close(cli);
    close(srv);
    return 0;
}

int main(int argc, char** argv) {
    const char* port = argc > 1 ? argv[1] : "7000";

    if (argc > 2 && !strcmp(argv[2], "blocking")) {
        unsigned long requests = argc > 3 ? strtoul(argv[3], NULL, 10) : 5000;
        return serve_blocking(port, requests);
    }
    if (argc > 2 && !strcmp(argv[2], "epoll")) {
        unsigned long requests = argc > 3 ? strtoul(argv[3], NULL, 10) : 5000;
        return serve_epoll(port, requests);
    }

    /* a real server (redis, nginx) survives a peer that disappears mid-write */
    signal(SIGPIPE, SIG_IGN);

    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) {
        perror("socket");
        return 1;
    }
    int one = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)atoi(port));
    if (bind(srv, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        return 1;
    }
    if (listen(srv, 32) < 0) {
        perror("listen");
        return 1;
    }
    printf("LISTENING %s\n", port);
    fflush(stdout);

    size_t cap = 1u << 20;
    unsigned char* buf = malloc(cap);
    if (!buf) {
        perror("malloc");
        return 1;
    }

    unsigned long count = 0;
    for (;;) {
        int cli = accept(srv, NULL, NULL);
        if (cli < 0) {
            perror("accept");
            return 1;
        }
        size_t len = 0;
        if (read_all(cli, buf, cap, &len) < 0) {
            printf("ERROR accept#%lu: read failed: %s\n", count, strerror(errno));
            fflush(stdout);
            close(cli);
            continue; /* stay up: a dead connection must not end the test */
        }
        if (len == 7 && memcmp(buf, "GOODBYE", 7) == 0) {
            close(cli);
            printf("DONE %lu\n", count);
            fflush(stdout);
            return 0;
        }
        {
            /* what this connection carries: one request (seq, len == size+4) or several merged
             * ones (a delivery bug); says what the client will check against */
            uint32_t seq = len >= 4 ? (uint32_t)buf[0] | buf[1] << 8 | buf[2] << 16 |
                                      (uint32_t)buf[3] << 24
                                    : 0xffffffff;
            printf("SERVE accept#%lu seq %u len %zu\n", count, seq, len);
            fflush(stdout);
        }
        if (write_all(cli, buf, len) < 0) {
            uint32_t seq = len >= 4 ? (uint32_t)buf[0] | buf[1] << 8 | buf[2] << 16 |
                                      (uint32_t)buf[3] << 24
                                    : 0xffffffff;
            printf("ERROR accept#%lu: echo write failed for seq %u (errno %d)\n",
                   count, seq, errno);
            fflush(stdout);
            close(cli);
            continue;
        }
        count++;
        close(cli);
    }
}
