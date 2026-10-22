/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (C) 2026 The ASTERISC Research Lab */
/* Microbenchmark: per-call cost of raw syscalls (glibc's inline `svc #0` / `syscall`).
 * Run natively and under saguaro-direct to measure the interception overhead. */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

static double now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e9 + ts.tv_nsec;
}

static int fd_zero, fd_null;

static void op_getpid(void) { syscall(SYS_getpid); }
static void op_getuid(void) { syscall(SYS_getuid); }
static void op_read_zero(void) { char c; if (read(fd_zero, &c, 1) < 0) abort(); }
static void op_write_null(void) { char c = 0; if (write(fd_null, &c, 1) < 0) abort(); }
static void op_clock_raw(void) { struct timespec ts; syscall(SYS_clock_gettime, CLOCK_MONOTONIC, &ts); }

static const struct { const char* name; void (*fn)(void); } OPS[] = {
    {"getpid",         op_getpid},
    {"getuid",         op_getuid},
    {"read(/dev/zero,1)", op_read_zero},
    {"write(/dev/null,1)", op_write_null},
    {"clock_gettime(raw)", op_clock_raw},
};

int main(int argc, char** argv) {
    long iters = argc > 1 ? atol(argv[1]) : 200000;
    int reps = argc > 2 ? atoi(argv[2]) : 5;
    fd_zero = open("/dev/zero", O_RDONLY);
    fd_null = open("/dev/null", O_WRONLY);
    if (fd_zero < 0 || fd_null < 0) { perror("open"); return 1; }

    printf("iters=%ld reps=%d (ns/call, min and median of reps)\n", iters, reps);
    for (size_t i = 0; i < sizeof(OPS) / sizeof(OPS[0]); i++) {
        double res[64];
        if (reps > 64) reps = 64;
        for (long w = 0; w < iters / 10; w++) OPS[i].fn(); /* warmup */
        for (int r = 0; r < reps; r++) {
            double t0 = now_ns();
            for (long k = 0; k < iters; k++) OPS[i].fn();
            res[r] = (now_ns() - t0) / iters;
        }
        for (int a = 0; a < reps; a++)
            for (int b = a + 1; b < reps; b++)
                if (res[b] < res[a]) { double t = res[a]; res[a] = res[b]; res[b] = t; }
        printf("%-22s min %9.1f  median %9.1f\n", OPS[i].name, res[0], res[reps / 2]);
    }
    return 0;
}
