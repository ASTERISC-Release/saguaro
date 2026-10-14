/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/*
 * Regression tests for timerfd beyond the pinned LTP: one-shot and periodic expirations with the
 * overrun counter, coalescing of periodic expirations between reads, disarm, the reported state of
 * `timerfd_gettime()`/`timerfd_settime()` (including the old value), nonblocking reads, and waking
 * `poll()`/`epoll()` from the expiration worker.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

#define CHECK(cond)                                                \
    do {                                                           \
        if (!(cond)) {                                             \
            fprintf(stderr, "%s:%d: %s failed (errno=%d)\n",       \
                    __FILE__, __LINE__, #cond, errno);             \
            exit(1);                                               \
        }                                                          \
    } while (0)

static uint64_t read_expirations(int fd) {
    uint64_t buf = 0;
    ssize_t n = read(fd, &buf, sizeof(buf));
    CHECK(n == (ssize_t)sizeof(buf));
    return buf;
}

static void set_oneshot(int fd, int seconds) {
    struct itimerspec its = {
        .it_value = { .tv_sec = seconds },
    };
    CHECK(timerfd_settime(fd, 0, &its, NULL) == 0);
}

int main(void) {
    /* creation and argument validation */
    CHECK(timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC) >= 0);
    CHECK(timerfd_create(-1, 0) == -1 && errno == EINVAL);
    CHECK(timerfd_create(CLOCK_MONOTONIC, 12345) == -1 && errno == EINVAL);

    int fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
    CHECK(fd >= 0);

    /* a fresh timer reports disarmed and reads nothing */
    struct itimerspec its;
    CHECK(timerfd_gettime(fd, &its) == 0);
    CHECK(its.it_value.tv_sec == 0 && its.it_value.tv_nsec == 0);
    CHECK(its.it_interval.tv_sec == 0 && its.it_interval.tv_nsec == 0);
    CHECK(read(fd, &its, sizeof(uint64_t)) == -1 && errno == EAGAIN);

    /* bad arguments on a live fd */
    CHECK(timerfd_settime(fd, 8, &its, NULL) == -1 && errno == EINVAL);
    CHECK(timerfd_settime(fd, 0, NULL, (struct itimerspec*)PATH_MAX) == -1 && errno == EFAULT);
    int non_fd = STDERR_FILENO;
    CHECK(timerfd_settime(non_fd, 0, &its, NULL) == -1 && errno == EINVAL);
    CHECK(timerfd_gettime(non_fd, &its) == -1 && errno == EINVAL);

    /* one short interval, armed relatively; the old value is reported empty */
    struct itimerspec old;
    struct itimerspec one = { .it_value = { .tv_sec = 5 } };
    CHECK(timerfd_settime(fd, 0, &one, &old) == 0);
    CHECK(old.it_value.tv_sec == 0 && old.it_interval.tv_sec == 0);

    /* `timerfd_gettime()` counts down (in this LibOS all clocks are the system clock) */
    CHECK(timerfd_gettime(fd, &its) == 0);
    CHECK(its.it_value.tv_sec == 5 || its.it_value.tv_sec == 4);
    CHECK(read(fd, &its, sizeof(uint64_t)) == -1 && errno == EAGAIN); /* not yet expired */

    /* disarm through `timerfd_settime()` */
    struct itimerspec zero = {0};
    CHECK(timerfd_settime(fd, 0, &zero, &old) == 0);
    CHECK(old.it_value.tv_sec >= 4); /* what remained when disarmed */
    CHECK(timerfd_gettime(fd, &its) == 0);
    CHECK(its.it_value.tv_sec == 0 && its.it_interval.tv_sec == 0);

    /* absolute expiry in the past fires immediately with a count of one */
    struct timespec now;
    CHECK(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
    struct itimerspec past = { .it_value = { .tv_sec = now.tv_sec - 1 } };
    CHECK(timerfd_settime(fd, TFD_TIMER_ABSTIME, &past, NULL) == 0);
    CHECK(read_expirations(fd) == 1);

    /* absolute expiry in the future is reported absolutely with `TFD_TIMER_ABSTIME` */
    struct itimerspec future = { .it_value = { .tv_sec = now.tv_sec + 100 } };
    CHECK(timerfd_settime(fd, TFD_TIMER_ABSTIME, &future, NULL) == 0);
    CHECK(timerfd_gettime(fd, &its) == 0);
    CHECK(its.it_value.tv_sec > 90);
    CHECK(timerfd_settime(fd, 0, &zero, NULL) == 0);

    /* `poll()` wakes up when the timer expires (the expiration worker thread) */
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    set_oneshot(fd, 1);
    CHECK(poll(&pfd, 1, 10000) == 1);
    CHECK(pfd.revents & POLLIN);
    CHECK(read_expirations(fd) == 1);

    /* the blocking read returns when the timer fires, with the overrun count */
    int bfd = timerfd_create(CLOCK_MONOTONIC, 0);
    CHECK(bfd >= 0);
    set_oneshot(bfd, 1);
    CHECK(read_expirations(bfd) == 1);

    /* periodic timer: several short periods accumulate one burst if not read in between, and the
     * interval survives (`timerfd_gettime()` keeps reporting it) */
    struct itimerspec period = {
        .it_interval = { .tv_nsec = 10 * 1000 * 1000 },  /* 10 ms */
        .it_value    = { .tv_nsec = 20 * 1000 * 1000 },  /* first in 20 ms */
    };
    struct timespec t0, t1;
    CHECK(timerfd_settime(bfd, 0, &period, NULL) == 0);
    CHECK(timerfd_gettime(bfd, &its) == 0);
    CHECK(its.it_interval.tv_nsec == 10 * 1000 * 1000);
    long sleep_us = 350 * 1000;
    CHECK(clock_gettime(CLOCK_MONOTONIC, &t0) == 0);
    usleep(sleep_us); /* ~35 expirations, none read */
    CHECK(clock_gettime(CLOCK_MONOTONIC, &t1) == 0);
    uint64_t expirations = read_expirations(bfd);
    /* at least the expirations that certainly happened in that window, at most the maximum the
     * interval could produce from the same measured wall time (clock granularity tolerant) */
    uint64_t measure_ns = (uint64_t)(t1.tv_sec - t0.tv_sec) * 1000000000
                          + (uint64_t)(t1.tv_nsec - t0.tv_nsec);
    CHECK(expirations >= 20);
    CHECK(expirations <= measure_ns / (10 * 1000 * 1000) + 3);
    /* the next expiration is pending soon and readable without blocking the test forever */
    struct pollfd nextfd = { .fd = bfd, .events = POLLIN };
    CHECK(poll(&nextfd, 1, 10000) == 1);

    /* `epoll()` sees the expiration too */
    int epfd = epoll_create1(0);
    CHECK(epfd >= 0);
    struct epoll_event ev = { .events = EPOLLIN, .data.fd = bfd };
    CHECK(epoll_ctl(epfd, EPOLL_CTL_ADD, bfd, &ev) == 0);
    CHECK(read_expirations(bfd) >= 0); /* drain, then wait for the next period */
    CHECK(epoll_wait(epfd, &ev, 1, 10000) == 1);
    CHECK(ev.data.fd == bfd && (ev.events & EPOLLIN));
    read_expirations(bfd);

    /* disarm the periodic timer: the timer reports disarmed, with the interval kept */
    CHECK(timerfd_settime(bfd, 0, &zero, NULL) == 0);
    CHECK(timerfd_gettime(bfd, &its) == 0);
    CHECK(its.it_value.tv_sec == 0 && its.it_value.tv_nsec == 0);
    CHECK(its.it_interval.tv_nsec == 10 * 1000 * 1000);

    /* writes to a timerfd are refused with EINVAL, like on the kernel */
    uint64_t val = 1;
    CHECK(write(bfd, &val, sizeof(val)) == -1 && errno == EINVAL);

    printf("TEST OK\n");
    return 0;
}
