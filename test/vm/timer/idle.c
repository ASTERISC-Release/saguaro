/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */
/* An idle guest process for counting timer interrupts (test/vm-timer/measure-sev.sh): prints READY,
 * then sleeps for argv[1] seconds either in one nanosleep (argv[2] = 0, the default) or in steps of
 * argv[2] ms (a periodic timeout every step), prints DONE and exits. */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

int main(int argc, char** argv) {
    long secs = argc > 1 ? atol(argv[1]) : 30;
    long step_ms = argc > 2 ? atol(argv[2]) : 0;
    setvbuf(stdout, NULL, _IONBF, 0);
    puts("READY");
    struct timespec t0, t;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (;;) {
        clock_gettime(CLOCK_MONOTONIC, &t);
        double left = secs - ((t.tv_sec - t0.tv_sec) + (t.tv_nsec - t0.tv_nsec) / 1e9);
        if (left <= 0)
            break;
        struct timespec ts;
        if (step_ms > 0 && left * 1000 > step_ms) {
            ts.tv_sec = step_ms / 1000;
            ts.tv_nsec = (step_ms % 1000) * 1000000L;
        } else {
            ts.tv_sec = (time_t)left;
            ts.tv_nsec = (long)((left - (time_t)left) * 1e9);
        }
        nanosleep(&ts, NULL);
    }
    puts("DONE");
    return 0;
}
