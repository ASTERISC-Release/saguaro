/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/* Regression: a guest TCP client to 127.0.0.1 must reach a server in the same guest. */
#include <arpa/inet.h>
#include <stdbool.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define REQUEST_COUNT 1000
#define REQUEST_SIZE 64

struct server_args {
    int family;
    uint16_t port;
    pthread_mutex_t lock;
    pthread_cond_t ready_cond;
    bool ready;
    int error;
};

static int transfer_all(int fd, void* data, size_t size, bool sending) {
    size_t offset = 0;
    while (offset < size) {
        ssize_t count = sending ? send(fd, (char*)data + offset, size - offset, 0)
                                : recv(fd, (char*)data + offset, size - offset, 0);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            return -1;
        offset += (size_t)count;
    }
    return 0;
}

static void* loopback_server(void* arg) {
    struct server_args* server = arg;
    int listener = socket(server->family, SOCK_STREAM, 0);
    if (listener < 0)
        goto failed;

    int one = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_storage address = {0};
    socklen_t address_size;
    if (server->family == AF_INET) {
        struct sockaddr_in* addr4 = (struct sockaddr_in*)&address;
        addr4->sin_family = AF_INET;
        addr4->sin_port = htons(server->port);
        addr4->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address_size = sizeof(*addr4);
    } else {
        struct sockaddr_in6* addr6 = (struct sockaddr_in6*)&address;
        addr6->sin6_family = AF_INET6;
        addr6->sin6_port = htons(server->port);
        addr6->sin6_addr = in6addr_loopback;
        address_size = sizeof(*addr6);
    }
    if (bind(listener, (struct sockaddr*)&address, address_size) < 0 || listen(listener, 8) < 0) {
        close(listener);
        goto failed;
    }

    pthread_mutex_lock(&server->lock);
    server->ready = true;
    pthread_cond_signal(&server->ready_cond);
    pthread_mutex_unlock(&server->lock);

    int client = accept(listener, NULL, NULL);
    close(listener);
    if (client < 0)
        return NULL;

    for (unsigned int i = 0; i < REQUEST_COUNT; i++) {
        unsigned char request[REQUEST_SIZE];
        if (transfer_all(client, request, sizeof(request), false) < 0 ||
                transfer_all(client, request, sizeof(request), true) < 0) {
            close(client);
            return NULL;
        }
    }
    close(client);
    return NULL;

failed:
    pthread_mutex_lock(&server->lock);
    server->error = errno ? errno : EIO;
    server->ready = true;
    pthread_cond_signal(&server->ready_cond);
    pthread_mutex_unlock(&server->lock);
    return NULL;
}

static int test_tcp_loopback(int family, uint16_t port) {
    struct server_args server = {
        .family = family,
        .port = port,
        .lock = PTHREAD_MUTEX_INITIALIZER,
        .ready_cond = PTHREAD_COND_INITIALIZER,
    };
    pthread_t server_thread;
    int ret = pthread_create(&server_thread, NULL, loopback_server, &server);
    if (ret) {
        fprintf(stderr, "pthread_create: %s\n", strerror(ret));
        return -1;
    }

    pthread_mutex_lock(&server.lock);
    while (!server.ready)
        pthread_cond_wait(&server.ready_cond, &server.lock);
    int server_error = server.error;
    pthread_mutex_unlock(&server.lock);
    if (server_error) {
        fprintf(stderr, "loopback server setup: %s\n", strerror(server_error));
        pthread_join(server_thread, NULL);
        return -1;
    }

    int client = socket(family, SOCK_STREAM, 0);
    if (client < 0) {
        perror("client socket");
        return -1;
    }
    struct sockaddr_storage address = {0};
    socklen_t address_size;
    if (family == AF_INET) {
        struct sockaddr_in* addr4 = (struct sockaddr_in*)&address;
        addr4->sin_family = AF_INET;
        addr4->sin_port = htons(port);
        addr4->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address_size = sizeof(*addr4);
    } else {
        struct sockaddr_in6* addr6 = (struct sockaddr_in6*)&address;
        addr6->sin6_family = AF_INET6;
        addr6->sin6_port = htons(port);
        addr6->sin6_addr = in6addr_loopback;
        address_size = sizeof(*addr6);
    }
    if (connect(client, (struct sockaddr*)&address, address_size) < 0) {
        perror("guest loopback connect");
        close(client);
        return -1;
    }

    for (unsigned int i = 0; i < REQUEST_COUNT; i++) {
        unsigned char request[REQUEST_SIZE];
        for (size_t j = 0; j < sizeof(request); j++)
            request[j] = (unsigned char)(i + j);
        unsigned char response[REQUEST_SIZE];
        if (transfer_all(client, request, sizeof(request), true) < 0 ||
                transfer_all(client, response, sizeof(response), false) < 0 ||
                memcmp(request, response, sizeof(request))) {
            fprintf(stderr, "loopback echo mismatch at request %u\n", i);
            close(client);
            pthread_join(server_thread, NULL);
            return -1;
        }
    }

    close(client);
    pthread_join(server_thread, NULL);
    pthread_cond_destroy(&server.ready_cond);
    pthread_mutex_destroy(&server.lock);
    return 0;
}

static int test_udp_loopback(int family, uint16_t port) {
    int fd = socket(family, SOCK_DGRAM, 0);
    if (fd < 0) {
        perror("UDP socket");
        return -1;
    }

    struct sockaddr_storage address = {0};
    socklen_t address_size;
    if (family == AF_INET) {
        struct sockaddr_in* addr4 = (struct sockaddr_in*)&address;
        addr4->sin_family = AF_INET;
        addr4->sin_port = htons(port);
        addr4->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address_size = sizeof(*addr4);
    } else {
        struct sockaddr_in6* addr6 = (struct sockaddr_in6*)&address;
        addr6->sin6_family = AF_INET6;
        addr6->sin6_port = htons(port);
        addr6->sin6_addr = in6addr_loopback;
        address_size = sizeof(*addr6);
    }
    if (bind(fd, (struct sockaddr*)&address, address_size) < 0) {
        perror("UDP loopback bind");
        close(fd);
        return -1;
    }

    static const char request[] = "guest UDP loopback";
    if (sendto(fd, request, sizeof(request), 0, (struct sockaddr*)&address, address_size) !=
            (ssize_t)sizeof(request)) {
        perror("UDP loopback sendto");
        close(fd);
        return -1;
    }
    char response[64] = {0};
    ssize_t count = recvfrom(fd, response, sizeof(response), 0, NULL, NULL);
    close(fd);
    if (count != (ssize_t)sizeof(request) || memcmp(response, request, sizeof(request))) {
        fprintf(stderr, "UDP loopback returned %zd bytes, expected %zu\n", count,
                sizeof(request));
        return -1;
    }
    return 0;
}

int main(void) {
    if (test_tcp_loopback(AF_INET, 28091) < 0 ||
            test_tcp_loopback(AF_INET6, 28092) < 0 ||
            test_udp_loopback(AF_INET, 28093) < 0 ||
            test_udp_loopback(AF_INET6, 28094) < 0)
        return 1;

    puts("GUEST_LOOPBACK_OK tcp4=1000 tcp6=1000 udp4=1 udp6=1");
    return 0;
}
