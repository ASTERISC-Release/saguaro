/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/*
 * A host UNIX socket (sys.insecure__host_unix_sockets, saguaro-direct): connect to a server that
 * runs on the host (started by the test harness), pass it an fd of a file opened in Gramine
 * (SCM_RIGHTS, LibOS fd -> host fd), and receive an fd from it (host fd -> new LibOS fd); then pass
 * it one end of a socketpair and receive an fd through the other end.
 *
 * The server reads the file through the fd it got and answers with the read end of a pipe that
 * holds a greeting.
 */

#define _GNU_SOURCE
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define SOCK_PATH "/tmp/saguaro_host_unix_test.sock"
#define FILE_TEXT "hello from gramine"
#define HOST_TEXT "hello from the host"

int main(int argc, char** argv) {
    if (argc < 2)
        errx(1, "usage: %s <scratch file>", argv[0]);

    /* the fd to pass: a file in a chroot mount (backed by a host file) */
    int file_fd = open(argv[1], O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (file_fd < 0)
        err(1, "open %s", argv[1]);
    if (write(file_fd, FILE_TEXT, strlen(FILE_TEXT)) != (ssize_t)strlen(FILE_TEXT))
        err(1, "write");
    if (lseek(file_fd, 0, SEEK_SET) != 0)
        err(1, "lseek");

    int s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (s < 0)
        err(1, "socket");
    struct sockaddr_un addr = {.sun_family = AF_UNIX, .sun_path = SOCK_PATH};
    if (connect(s, (struct sockaddr*)&addr, sizeof(addr)) < 0)
        err(1, "connect %s", SOCK_PATH);

    /* send "FD" with the file's fd; the control buffer ends without padding, as libxcb does */
    char cbuf[CMSG_SPACE(sizeof(int))] = {0};
    struct iovec iov = {.iov_base = (char*)"FD", .iov_len = 2};
    struct msghdr msg = {.msg_iov = &iov, .msg_iovlen = 1, .msg_control = cbuf,
                         .msg_controllen = CMSG_LEN(sizeof(int))};
    struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cmsg), &file_fd, sizeof(int));
    if (sendmsg(s, &msg, 0) != 2)
        err(1, "sendmsg with SCM_RIGHTS");

    /* the answer: "OK" with a pipe's read end */
    struct pollfd pfd = {.fd = s, .events = POLLIN};
    if (poll(&pfd, 1, 10000) != 1)
        errx(1, "poll on the host socket: no answer");
    char buf[64] = {0};
    char rbuf[CMSG_SPACE(4 * sizeof(int))] = {0};
    iov = (struct iovec){.iov_base = buf, .iov_len = sizeof(buf) - 1};
    msg = (struct msghdr){.msg_iov = &iov, .msg_iovlen = 1, .msg_control = rbuf,
                          .msg_controllen = sizeof(rbuf)};
    ssize_t n = recvmsg(s, &msg, 0);
    if (n < 0)
        err(1, "recvmsg");
    if (n != 2 || memcmp(buf, "OK", 2))
        errx(1, "unexpected answer '%s' (%zd bytes)", buf, n);
    cmsg = CMSG_FIRSTHDR(&msg);
    if (!cmsg || cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS ||
            cmsg->cmsg_len != CMSG_LEN(sizeof(int)))
        errx(1, "no fd in the answer");
    int pipe_fd;
    memcpy(&pipe_fd, CMSG_DATA(cmsg), sizeof(int));

    memset(buf, 0, sizeof(buf));
    n = read(pipe_fd, buf, sizeof(buf) - 1);
    if (n != (ssize_t)strlen(HOST_TEXT) || memcmp(buf, HOST_TEXT, n))
        errx(1, "read from the received fd: '%s' (%zd bytes)", buf, n);

    close(pipe_fd);

    /* a socketpair end passed to the host: the server answers through it with "PAIR" and another
     * pipe's read end (what the NVIDIA GLX library does with the X server) */
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) < 0)
        err(1, "socketpair");
    memset(cbuf, 0, sizeof(cbuf));
    iov = (struct iovec){.iov_base = (char*)"SP", .iov_len = 2};
    msg = (struct msghdr){.msg_iov = &iov, .msg_iovlen = 1, .msg_control = cbuf,
                          .msg_controllen = CMSG_SPACE(sizeof(int))};
    cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cmsg), &sv[1], sizeof(int));
    if (sendmsg(s, &msg, 0) != 2)
        err(1, "sendmsg of a socketpair end");
    close(sv[1]);

    memset(buf, 0, sizeof(buf));
    memset(rbuf, 0, sizeof(rbuf));
    iov = (struct iovec){.iov_base = buf, .iov_len = sizeof(buf) - 1};
    msg = (struct msghdr){.msg_iov = &iov, .msg_iovlen = 1, .msg_control = rbuf,
                          .msg_controllen = sizeof(rbuf)};
    n = recvmsg(sv[0], &msg, 0);
    if (n != 4 || memcmp(buf, "PAIR", 4))
        errx(1, "unexpected answer on the socketpair '%s' (%zd bytes)", buf, n);
    cmsg = CMSG_FIRSTHDR(&msg);
    if (!cmsg || cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS ||
            cmsg->cmsg_len != CMSG_LEN(sizeof(int)))
        errx(1, "no fd in the answer on the socketpair");
    memcpy(&pipe_fd, CMSG_DATA(cmsg), sizeof(int));
    memset(buf, 0, sizeof(buf));
    n = read(pipe_fd, buf, sizeof(buf) - 1);
    if (n != (ssize_t)strlen(HOST_TEXT) || memcmp(buf, HOST_TEXT, n))
        errx(1, "read from the fd received on the socketpair: '%s' (%zd bytes)", buf, n);
    close(pipe_fd);
    close(sv[0]);

    close(s);
    close(file_fd);
    unlink(argv[1]);
    puts("TEST OK");
    return 0;
}
