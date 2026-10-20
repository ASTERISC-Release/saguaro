/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/* The timer probes of test/proton-vm/probes.c (tickgran, threadlat, pipeping), verbatim, as a
 * program of their own: probes.c includes x86-only headers and code and does not compile on
 * aarch64. Used by test/vm/timer/measure.sh through test/proton-vm/run-probes.sh (PROBES_SRC).
 * Usage: timerprobes PROBE... (the output lines are those of probes.c). */
#define _GNU_SOURCE
#include <errno.h>
#include <linux/futex.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#define P(name, ...) do { printf("PROBE " name ": " __VA_ARGS__); printf("\n"); fflush(stdout); } while (0)
#define E() strerror(errno)

/* pipeping: the wait-events path of the VM PAL under contention. Two threads pass one byte back and
 * forth over two pipes, each side poll()ing its pipe before reading (poll -> PalStreamsWaitEvents
 * -> the PAL's streams-waiting futex, which a pipe write wakes). A wakeup lost between the
 * readiness check and the sleep hangs a side for ever; the watchdog reports the round. */
#define PP_ROUNDS 200000
static int pp_a[2], pp_b[2];
static volatile int pp_round;
static int pp_wait_read(int fd) {
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    for (;;) {
        int r = poll(&pfd, 1, -1);
        if (r < 0 && errno == EINTR) continue;
        if (r < 0) return -1;
        char c;
        ssize_t n = read(fd, &c, 1);
        if (n == 1) return 0;
        if (n < 0 && errno == EINTR) continue;
        return -1;
    }
}
static void* pp_peer(void* arg) {
    (void)arg;
    for (int r = 0; r < PP_ROUNDS; r++) {
        if (pp_wait_read(pp_a[0]) < 0) { P("pipeping", "FAIL peer read: %s", strerror(errno)); _exit(4); }
        if (write(pp_b[1], "b", 1) != 1) { P("pipeping", "FAIL peer write: %s", strerror(errno)); _exit(4); }
    }
    return NULL;
}
static void* pp_watchdog(void* arg) {
    (void)arg;
    sleep(90);
    P("pipeping", "FAIL hung at round %d of %d (90 s)", pp_round, PP_ROUNDS);
    _exit(3);
}
static void probe_pipeping(void) {
    if (pipe(pp_a) < 0 || pipe(pp_b) < 0) { P("pipeping", "FAIL pipe: %s", strerror(errno)); return; }
    pthread_t w, t;
    pthread_create(&w, NULL, pp_watchdog, NULL);
    pthread_create(&t, NULL, pp_peer, NULL);
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int r = 0; r < PP_ROUNDS; r++) {
        pp_round = r;
        if (write(pp_a[1], "a", 1) != 1) { P("pipeping", "FAIL write: %s", strerror(errno)); return; }
        if (pp_wait_read(pp_b[0]) < 0) { P("pipeping", "FAIL read: %s", strerror(errno)); return; }
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    pthread_join(t, NULL);
    P("pipeping", "PASS %d rounds in %.1f s", PP_ROUNDS,
      (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9);
}

/* threadlat: what a game's short deadlines are spent on. (a) create a thread that returns at
 * once and join it; (b) a pipe round trip to a peer thread that sleeps in poll() between messages
 * (its vCPU idles, as the wineserver's does between requests); 200 samples each, median and max. */
static int tl_cmp(const void* a, const void* b) { double x = *(const double*)a, y = *(const double*)b; return (x > y) - (x < y); }
static double tl_now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
static void* tl_noop(void* a) { (void)a; return NULL; }
static int tl_p[2], tl_q[2];
static void* tl_peer(void* a) {
    (void)a;
    struct pollfd pfd = { .fd = tl_p[0], .events = POLLIN };
    for (int i = 0; i < 200; i++) {
        while (poll(&pfd, 1, -1) < 0 && errno == EINTR) ;
        char c; if (read(tl_p[0], &c, 1) != 1) return NULL;
        if (write(tl_q[1], &c, 1) != 1) return NULL;
    }
    return NULL;
}
static void tl_report(const char* what, double* v, int n) {
    qsort(v, n, sizeof(*v), tl_cmp);
    P("threadlat", "INFO %s: median %.2f ms, p90 %.2f ms, max %.2f ms (%d samples)", what, v[n / 2], v[n * 9 / 10], v[n - 1], n);
}
static void probe_threadlat(void) {
    double v[200];
    for (int i = 0; i < 200; i++) {
        double t0 = tl_now(); pthread_t t; pthread_create(&t, NULL, tl_noop, NULL); pthread_join(t, NULL);
        v[i] = tl_now() - t0;
        usleep(3000); /* let the CPUs go idle between samples, as between a game's steps */
    }
    tl_report("thread create+join, CPUs idle between samples", v, 200);
    if (pipe(tl_p) < 0 || pipe(tl_q) < 0) { P("threadlat", "FAIL pipe"); return; }
    pthread_t peer; pthread_create(&peer, NULL, tl_peer, NULL);
    usleep(20000);
    for (int i = 0; i < 200; i++) {
        double t0 = tl_now(); char c = 'x', r;
        if (write(tl_p[1], &c, 1) != 1 || read(tl_q[0], &r, 1) != 1) { P("threadlat", "FAIL pipe round trip"); return; }
        v[i] = tl_now() - t0;
        if (v[i] > 1.0) P("threadlat", "INFO round trip sample %d took %.2f ms", i, v[i]);
        usleep(3000);
    }
    pthread_join(peer, NULL);
    tl_report("pipe round trip to a sleeping peer", v, 200);
}

/* tickgran: the granularity of short timed waits: nanosleep, a futex wait with a timeout, poll and
 * epoll_wait with 1 ms and 5 ms timeouts, 20 each; median and max. */
static double tg_now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
static void tg_report(const char* what, double* v, int n) {
    qsort(v, n, sizeof(*v), tl_cmp);
    P("tickgran", "INFO %-22s median %7.2f ms, max %7.2f ms", what, v[n / 2], v[n - 1]);
}
static void probe_tickgran(void) {
    double v[20]; int w = 0; int fds[2]; pipe(fds);
    int ep = epoll_create1(0); struct epoll_event ee = { .events = EPOLLIN, .data.fd = fds[0] }; epoll_ctl(ep, EPOLL_CTL_ADD, fds[0], &ee);
    for (int ms = 1; ms <= 5; ms += 4) {
        struct timespec ts = { 0, ms * 1000000L };
        for (int i = 0; i < 20; i++) { double t0 = tg_now(); nanosleep(&ts, NULL); v[i] = tg_now() - t0; }
        char name[32]; snprintf(name, sizeof(name), "nanosleep %d ms", ms); tg_report(name, v, 20);
        for (int i = 0; i < 20; i++) { double t0 = tg_now(); struct timespec rel = { 0, ms * 1000000L }; syscall(__NR_futex, &w, FUTEX_WAIT | FUTEX_PRIVATE_FLAG, 0, &rel, NULL, 0); v[i] = tg_now() - t0; }
        snprintf(name, sizeof(name), "futex wait %d ms", ms); tg_report(name, v, 20);
        struct pollfd pfd = { .fd = fds[0], .events = POLLIN };
        for (int i = 0; i < 20; i++) { double t0 = tg_now(); poll(&pfd, 1, ms); v[i] = tg_now() - t0; }
        snprintf(name, sizeof(name), "poll %d ms", ms); tg_report(name, v, 20);
        for (int i = 0; i < 20; i++) { double t0 = tg_now(); struct epoll_event out; epoll_wait(ep, &out, 1, ms); v[i] = tg_now() - t0; }
        snprintf(name, sizeof(name), "epoll_wait %d ms", ms); tg_report(name, v, 20);
    }
}

struct probe { const char* name; void (*fn)(void); };
static const struct probe probes[] = {
    {"pipeping", probe_pipeping}, {"threadlat", probe_threadlat}, {"tickgran", probe_tickgran},
};

int main(int argc, char** argv) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    for (int a = 1; a < argc; a++) {
        int found = 0;
        for (unsigned i = 0; i < sizeof(probes) / sizeof(probes[0]); i++)
            if (!strcmp(argv[a], probes[i].name)) { probes[i].fn(); found = 1; }
        if (!found) fprintf(stderr, "unknown probe %s\n", argv[a]);
    }
    printf("PROBES DONE\n");
    return 0;
}
