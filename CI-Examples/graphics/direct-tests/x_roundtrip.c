/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/*
 * x_roundtrip: latency of an X request with a reply (GetInputFocus), natively and under
 * saguaro-direct (sys.insecure__host_unix_sockets). Usage: x_roundtrip [iterations]
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <xcb/xcb.h>

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

int main(int argc, char** argv) {
    long iters = argc > 1 ? atol(argv[1]) : 20000;
    xcb_connection_t* c = xcb_connect(NULL, NULL);
    if (xcb_connection_has_error(c)) {
        printf("xcb_connect failed\n");
        return 1;
    }
    for (int round = 0; round < 2; round++) {
        uint64_t t0 = now_ns();
        for (long i = 0; i < iters; i++)
            free(xcb_get_input_focus_reply(c, xcb_get_input_focus(c), NULL));
        uint64_t t1 = now_ns();
        if (round == 1)
            printf("X round trip: %.1f us (%ld iterations)\n", (double)(t1 - t0) / iters / 1000.0,
                   iters);
    }
    xcb_disconnect(c);
    printf("TEST OK\n");
    return 0;
}
