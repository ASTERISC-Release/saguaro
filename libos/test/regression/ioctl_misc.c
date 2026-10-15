/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/*
 * ioctl()s the LibOS answers itself (LTP ioctl07, sockioctl01): RNDGETENTCNT on /dev/random and
 * /dev/urandom, SIOCATMARK on a stream socket (EFAULT for a bad pointer, ENOTTY on a datagram
 * socket), and FIONREAD on a /proc file.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <linux/random.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
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
    int fd = open("/dev/random", O_RDONLY);
    CHECK(fd >= 0);
    int ent = -1;
    CHECK(ioctl(fd, RNDGETENTCNT, &ent) == 0 && ent >= 0);
    CHECK(ioctl(fd, RNDGETENTCNT, NULL) == -1 && errno == EFAULT);
    FILE* f = fopen("/proc/sys/kernel/random/entropy_avail", "r");
    int avail_bits = -1;
    CHECK(f && fscanf(f, "%d", &avail_bits) == 1 && avail_bits >= 0);
    fclose(f);
    CHECK(ioctl(fd, 0x12345678, &ent) == -1 && errno == EINVAL); /* the random driver's answer */
    close(fd);
    fd = open("/dev/urandom", O_RDONLY);
    CHECK(fd >= 0);
    CHECK(ioctl(fd, RNDGETENTCNT, &ent) == 0);
    close(fd);

    int tcp = socket(AF_INET, SOCK_STREAM, 0);
    int udp = socket(AF_INET, SOCK_DGRAM, 0);
    CHECK(tcp >= 0 && udp >= 0);
    int mark = -1;
    CHECK(ioctl(tcp, SIOCATMARK, &mark) == 0 && mark == 0);
    CHECK(ioctl(tcp, SIOCATMARK, NULL) == -1 && errno == EFAULT);
    CHECK(ioctl(udp, SIOCATMARK, &mark) == -1 && errno == ENOTTY);
    close(tcp);
    close(udp);

    fd = open("/proc/self/status", O_RDONLY);
    CHECK(fd >= 0);
    int avail = -1;
    CHECK(ioctl(fd, FIONREAD, &avail) == 0 && avail >= 0);
    close(fd);

    puts("TEST OK");
    return 0;
}
