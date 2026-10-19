/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/* Guest regression for resolver UDP and one relayed UDP echo datagram. */
#include <arpa/inet.h>
#include <netdb.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int main(int argc, char** argv) {
    if (argc != 4) {
        fprintf(stderr, "usage: %s <dns-name> <echo-ipv4> <echo-port>\n", argv[0]);
        return 2;
    }

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        perror("socket");
        return 1;
    }
    struct sockaddr_in target = { .sin_family = AF_INET, .sin_port = htons((uint16_t)atoi(argv[3])) };
    if (inet_pton(AF_INET, argv[2], &target.sin_addr) != 1) {
        fprintf(stderr, "invalid echo address: %s\n", argv[2]);
        return 2;
    }

    static const char request[] = "gramine-vm-udp-echo";
    if (sendto(fd, request, sizeof(request), 0, (struct sockaddr*)&target, sizeof(target)) !=
            (ssize_t)sizeof(request)) {
        perror("sendto");
        return 1;
    }
    char reply[64] = {0};
    struct sockaddr_in source = {0};
    socklen_t source_size = sizeof(source);
    ssize_t received = recvfrom(fd, reply, sizeof(reply), 0, (struct sockaddr*)&source,
                                &source_size);
    if (received != (ssize_t)sizeof(request) || memcmp(reply, request, sizeof(request))) {
        fprintf(stderr, "UDP echo returned %zd bytes, expected %zu\n", received, sizeof(request));
        return 1;
    }
    if (source.sin_port != target.sin_port || source.sin_addr.s_addr != target.sin_addr.s_addr) {
        fprintf(stderr, "UDP source mismatch: %s:%u\n", inet_ntoa(source.sin_addr),
                ntohs(source.sin_port));
        return 1;
    }
    puts("UDP_ECHO_OK");

    close(fd);

    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_DGRAM };
    struct addrinfo* result = NULL;
    int ret = getaddrinfo(argv[1], NULL, &hints, &result);
    if (ret) {
        fprintf(stderr, "getaddrinfo(%s): %s\n", argv[1], gai_strerror(ret));
        return 1;
    }
    char resolved[INET_ADDRSTRLEN];
    struct sockaddr_in* resolved_addr = (struct sockaddr_in*)result->ai_addr;
    if (!inet_ntop(AF_INET, &resolved_addr->sin_addr, resolved, sizeof(resolved))) {
        perror("inet_ntop");
        freeaddrinfo(result);
        return 1;
    }
    printf("DNS_RESOLVED %s %s\n", argv[1], resolved);
    freeaddrinfo(result);
    return 0;
}
