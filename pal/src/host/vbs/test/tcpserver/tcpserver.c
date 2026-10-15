/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/* A small TCP server inside a VBS enclave (phase 3e): listens on 127.0.0.1:18080 through the
 * host's sockets, answers three clients ("echo from the enclave: <request>"), then exits 0. */

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define PORT 18080
#define CLIENTS 3

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    int s = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(PORT),
                             .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    if (bind(s, (struct sockaddr*)&a, sizeof(a)) < 0 || listen(s, 8) < 0) {
        perror("bind/listen");
        return 1;
    }
    printf("tcpserver: listening on 127.0.0.1:%d\n", PORT);
    int served = 0;
    for (int i = 0; i < CLIENTS; i++) {
        struct sockaddr_in peer;
        socklen_t pl = sizeof(peer);
        int c = accept(s, (struct sockaddr*)&peer, &pl);
        if (c < 0) {
            perror("accept");
            return 1;
        }
        char req[256];
        ssize_t n = recv(c, req, sizeof(req) - 1, 0);
        if (n < 0)
            n = 0;
        req[n] = 0;
        char rep[320];
        int m = snprintf(rep, sizeof(rep), "echo from the enclave: %s", req);
        send(c, rep, m, 0);
        close(c);
        served++;
        printf("tcpserver: client %d from %s:%d sent \"%s\"\n", served, inet_ntoa(peer.sin_addr),
               ntohs(peer.sin_port), req);
    }
    close(s);
    printf("tcpserver: served %d clients\n", served);
    return served == CLIENTS ? 0 : 1;
}
