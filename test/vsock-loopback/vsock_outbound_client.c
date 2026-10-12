/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/* Small synchronous request regression for guest-initiated TCP through the VM relay. */
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int transfer_all(int fd, void* data, size_t size, int writing) {
    unsigned char* ptr = data;
    while (size) {
        ssize_t ret = writing ? send(fd, ptr, size, 0) : recv(fd, ptr, size, 0);
        if (ret < 0 && errno == EINTR)
            continue;
        if (ret <= 0)
            return -1;
        ptr += ret;
        size -= (size_t)ret;
    }
    return 0;
}

int main(int argc, char** argv) {
    if (argc != 4) {
        fprintf(stderr, "usage: %s <ipv4> <port> <requests|deny>\n", argv[0]);
        return 2;
    }

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return 1;
    }
    struct sockaddr_in addr = { .sin_family = AF_INET, .sin_port = htons((uint16_t)atoi(argv[2])) };
    if (inet_pton(AF_INET, argv[1], &addr.sin_addr) != 1) {
        fprintf(stderr, "invalid IPv4 address: %s\n", argv[1]);
        return 2;
    }

    if (!strcmp(argv[3], "deny")) {
        if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
            fprintf(stderr, "denied connect unexpectedly succeeded\n");
            return 1;
        }
        if (errno != EACCES) {
            fprintf(stderr, "denied connect returned errno %d, expected EACCES (%d)\n",
                    errno, EACCES);
            return 1;
        }
        puts("DENIED EACCES");
        close(fd);
        return 0;
    }

    char* end;
    unsigned long requests = strtoul(argv[3], &end, 10);
    if (!requests || *end) {
        fprintf(stderr, "invalid request count: %s\n", argv[3]);
        return 2;
    }
    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("connect");
        return 1;
    }

    unsigned char request[64], reply[64];
    for (unsigned long i = 0; i < requests; i++) {
        memset(request, (int)(i & 0xff), sizeof(request));
        request[0] = (unsigned char)(i >> 24);
        request[1] = (unsigned char)(i >> 16);
        request[2] = (unsigned char)(i >> 8);
        request[3] = (unsigned char)i;
        if (transfer_all(fd, request, sizeof(request), 1) < 0 ||
                transfer_all(fd, reply, sizeof(reply), 0) < 0 ||
                memcmp(request, reply, sizeof(request))) {
            fprintf(stderr, "request %lu failed (errno=%d)\n", i + 1, errno);
            return 1;
        }
        if ((i + 1) % 100 == 0) {
            printf("OUTBOUND_HANDLED %lu\n", i + 1);
            fflush(stdout);
        }
    }
    close(fd);
    printf("OUTBOUND_DONE %lu\n", requests);
    return 0;
}
