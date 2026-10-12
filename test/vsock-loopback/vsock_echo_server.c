/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/*
 * vsock_echo_server -- regression driver for host-initiated connections to a guest AF_INET TCP
 * server under the VM PAL (Documentation/devel/vm-network/, phase 1 of the plan).
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

int main(int argc, char** argv) {
    const char* port = argc > 1 ? argv[1] : "7000";

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
