/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/*
 * SCM_RIGHTS between two Saguaro processes, as wineserver and its clients use it: a child passes a
 * pipe end, a file (open, positioned, then unlinked), an eventfd and a socket end over an AF_UNIX
 * socketpair and then over a named socket; the parent reads through the received fds; a message
 * read with a control buffer that is too small loses the surplus fds; a plain read() of a message
 * that carried fds drops them but delivers the data; the fds ride with their bytes (data sent
 * before them is read without them).
 */

#define _GNU_SOURCE
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include "common.h"

#define SOCK_PATH "tmp/unix_fdpass_socket"
#define FILE_PATH "tmp/unix_fdpass_file"

static void send_fds(int sock, const char* data, size_t len, int* fds, size_t n) {
    char control[CMSG_SPACE(sizeof(int) * 8)];
    struct iovec iov = {.iov_base = (void*)data, .iov_len = len};
    struct msghdr msg = {.msg_iov = &iov, .msg_iovlen = 1};
    if (n) {
        msg.msg_control = control;
        msg.msg_controllen = CMSG_SPACE(sizeof(int) * n);
        struct cmsghdr* c = CMSG_FIRSTHDR(&msg);
        c->cmsg_level = SOL_SOCKET;
        c->cmsg_type = SCM_RIGHTS;
        c->cmsg_len = CMSG_LEN(sizeof(int) * n);
        memcpy(CMSG_DATA(c), fds, sizeof(int) * n);
    }
    ssize_t r = sendmsg(sock, &msg, 0);
    if (r != (ssize_t)len)
        err(1, "sendmsg returned %zd of %zu", r, len);
}

static size_t recv_fds(int sock, char* data, size_t len, int* fds, size_t cap, size_t* out_n) {
    char control[CMSG_SPACE(sizeof(int) * 8)];
    struct iovec iov = {.iov_base = data, .iov_len = len};
    struct msghdr msg = {.msg_iov = &iov, .msg_iovlen = 1};
    if (cap) {
        /* exactly the length of `cap` fds (CMSG_SPACE would round one fd up to two on x86_64) */
        msg.msg_control = control;
        msg.msg_controllen = CMSG_LEN(sizeof(int) * cap);
    }
    ssize_t r = recvmsg(sock, &msg, 0);
    if (r < 0)
        err(1, "recvmsg");
    *out_n = 0;
    for (struct cmsghdr* c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c)) {
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
            size_t n = (c->cmsg_len - CMSG_LEN(0)) / sizeof(int);
            memcpy(fds, CMSG_DATA(c), sizeof(int) * n);
            *out_n = n;
        }
    }
    return r;
}

static void child(int sock) {
    /* 1. plain data first: the fds of the next message must not come with it */
    send_fds(sock, "pre", 3, NULL, 0);

    int pfd[2];
    CHECK(pipe(pfd));
    int file = CHECK(open(FILE_PATH, O_RDWR | O_CREAT | O_TRUNC, 0600));
    CHECK(write(file, "0123456789", 10));
    CHECK(lseek(file, 4, SEEK_SET));
    int efd = CHECK(eventfd(0, EFD_CLOEXEC));
    uint64_t one = 3;
    CHECK(write(efd, &one, 8));
    int sv[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv));

    int fds[4] = {pfd[0], file, efd, sv[1]};
    send_fds(sock, "four", 4, fds, 4);
    CHECK(write(pfd[1], "pipe-data", 9));
    CHECK(write(sv[0], "sock-data", 9));
    CHECK(unlink(FILE_PATH)); /* the parent reads it through the passed fd all the same */

    /* 2. a message whose fds the parent reads with a control buffer for one fd only */
    int two[2] = {pfd[0], efd};
    send_fds(sock, "two", 3, two, 2);

    /* 3. a message whose fds the parent drops with a plain read() */
    send_fds(sock, "drop", 4, two, 2);

    /* 4. after it, a message with one fd must still arrive intact */
    send_fds(sock, "last", 4, &pfd[0], 1);

    /* keep the ends alive until the parent is done */
    char ack;
    CHECK(read(sock, &ack, 1));
    CHECK(write(pfd[1], "x", 1));
    exit(0);
}

static void parent(int sock) {
    char buf[64];
    int fds[8];
    size_t n = 0;

    size_t r = recv_fds(sock, buf, 3, fds, 4, &n);
    if (r != 3 || memcmp(buf, "pre", 3) || n != 0)
        errx(1, "the plain message came wrong: %zu bytes, %zu fds", r, n);

    r = recv_fds(sock, buf, sizeof(buf), fds, 4, &n);
    if (r != 4 || memcmp(buf, "four", 4) || n != 4)
        errx(1, "the message with four fds came wrong: %zu bytes, %zu fds", r, n);
    int rpipe = fds[0], rfile = fds[1], refd = fds[2], rsock = fds[3];

    memset(buf, 0, sizeof(buf));
    CHECK(read(rpipe, buf, 9));
    if (memcmp(buf, "pipe-data", 9))
        errx(1, "the passed pipe end does not read what the child wrote");

    struct stat st;
    CHECK(fstat(rfile, &st));
    if (!S_ISREG(st.st_mode) || st.st_size != 10)
        errx(1, "the passed file is not the regular 10-byte file (mode %#o size %ld)",
             st.st_mode, (long)st.st_size);
    memset(buf, 0, sizeof(buf));
    ssize_t fr = CHECK(read(rfile, buf, 6));
    if (fr != 6 || memcmp(buf, "456789", 6))
        errx(1, "the passed file does not continue at the sender's position (got %zd '%s')", fr, buf);

    uint64_t val = 0;
    CHECK(read(refd, &val, 8));
    if (val != 3)
        errx(1, "the passed eventfd does not hold the sender's count (%lu)", (unsigned long)val);

    memset(buf, 0, sizeof(buf));
    CHECK(read(rsock, buf, 9));
    if (memcmp(buf, "sock-data", 9))
        errx(1, "the passed socket end does not read what the child wrote");

    /* control buffer for one fd: one arrives, the other is dropped */
    r = recv_fds(sock, buf, sizeof(buf), fds, 1, &n);
    if (r != 3 || memcmp(buf, "two", 3) || n != 1)
        errx(1, "the message read with room for one fd came wrong: %zu bytes, %zu fds", r, n);
    CHECK(close(fds[0]));

    /* plain read(): the data arrives, the fds are dropped */
    memset(buf, 0, sizeof(buf));
    ssize_t pr = CHECK(read(sock, buf, sizeof(buf)));
    if (pr != 4 || memcmp(buf, "drop", 4))
        errx(1, "the plain read of the fd message came wrong: %zd bytes '%s'", pr, buf);

    r = recv_fds(sock, buf, sizeof(buf), fds, 4, &n);
    if (r != 4 || memcmp(buf, "last", 4) || n != 1)
        errx(1, "the last message came wrong: %zu bytes, %zu fds", r, n);
    CHECK(write(sock, "k", 1));
    memset(buf, 0, sizeof(buf));
    CHECK(read(fds[0], buf, 1));
    if (buf[0] != 'x')
        errx(1, "the pipe end passed last does not work");

    CHECK(close(fds[0]));
    CHECK(close(rpipe));
    CHECK(close(rfile));
    CHECK(close(refd));
    CHECK(close(rsock));
}

static void run_pair(int parent_sock, int child_sock) {
    fflush(stdout);
    pid_t pid = CHECK(fork());
    if (pid == 0) {
        CHECK(close(parent_sock));
        child(child_sock);
    }
    CHECK(close(child_sock));
    parent(parent_sock);
    int status = 0;
    CHECK(waitpid(pid, &status, 0));
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        errx(1, "child failed: status %#x", status);
    CHECK(close(parent_sock));
}

int main(void) {
    /* over a socketpair */
    int sv[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv));
    run_pair(sv[0], sv[1]);
    puts("socketpair OK");

    /* over a named socket: the child connects to the parent's listener */
    unlink(SOCK_PATH);
    int srv = CHECK(socket(AF_UNIX, SOCK_STREAM, 0));
    struct sockaddr_un sa = {.sun_family = AF_UNIX, .sun_path = SOCK_PATH};
    CHECK(bind(srv, (void*)&sa, sizeof(sa)));
    CHECK(listen(srv, 1));
    fflush(stdout);
    pid_t pid = CHECK(fork());
    if (pid == 0) {
        CHECK(close(srv));
        int c = CHECK(socket(AF_UNIX, SOCK_STREAM, 0));
        CHECK(connect(c, (void*)&sa, sizeof(sa)));
        child(c);
    }
    int conn = CHECK(accept(srv, NULL, NULL));
    parent(conn);
    int status = 0;
    CHECK(waitpid(pid, &status, 0));
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        errx(1, "child failed: status %#x", status);
    CHECK(close(conn));
    CHECK(close(srv));
    CHECK(unlink(SOCK_PATH));
    puts("named socket OK");

    puts("TEST OK");
    return 0;
}
