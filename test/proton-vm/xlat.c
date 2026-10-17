/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/* The latency of X round trips as a Wine program sees them: GetCursorPos() is an XQueryPointer
 * round trip in winex11, so each call crosses the guest's X connection (the vsock relay under
 * gramine-vm) and back. ROUNDS calls, 3 ms apart; median, p90, max and the count above 50 ms.
 * The thread first opens its own X connection (the first call), timed separately: that is what
 * every new Wine thread does on its first user32 call (FINDINGS.md of proton-vm, 2026-10-17).
 * Usage: xlat.exe [rounds] [threads]   (defaults 500 and 1: with N > 1, N threads run the loop at
 * once, each opening its own X connection on its first call, as every new Wine thread does) */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

struct worker { int rounds; double first, med, p90, max; int slow; HANDLE th; };
static int cmp(const void* a, const void* b);
static DWORD WINAPI worker_main(LPVOID arg) {
    struct worker* w = arg;
    double* v = malloc(sizeof(double) * w->rounds);
    LARGE_INTEGER f, t0, t1; POINT p;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0); GetCursorPos(&p); QueryPerformanceCounter(&t1);
    w->first = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)f.QuadPart;
    for (int i = 0; i < w->rounds; i++) {
        QueryPerformanceCounter(&t0); GetCursorPos(&p); QueryPerformanceCounter(&t1);
        v[i] = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)f.QuadPart;
        if (v[i] > 50.0) w->slow++;
        Sleep(3);
    }
    qsort(v, w->rounds, sizeof(*v), cmp);
    w->med = v[w->rounds / 2]; w->p90 = v[w->rounds * 9 / 10]; w->max = v[w->rounds - 1];
    return 0;
}

static int cmp(const void* a, const void* b) {
    double x = *(const double*)a, y = *(const double*)b;
    return (x > y) - (x < y);
}

int main(int argc, char** argv) {
    int rounds = argc > 1 ? atoi(argv[1]) : 500;
    int threads = argc > 2 ? atoi(argv[2]) : 1;
    if (rounds <= 0 || rounds > 100000) rounds = 500;
    if (threads > 1) {
        if (threads > 16) threads = 16;
        struct worker w[16] = {{0}};
        POINT p; GetCursorPos(&p); /* the main thread's connection first, as in a game */
        for (int i = 0; i < threads; i++) { w[i].rounds = rounds; w[i].th = CreateThread(NULL, 0, worker_main, &w[i], 0, NULL); }
        int slow = 0; double worst = 0, first_max = 0;
        for (int i = 0; i < threads; i++) {
            WaitForSingleObject(w[i].th, INFINITE);
            slow += w[i].slow; if (w[i].max > worst) worst = w[i].max; if (w[i].first > first_max) first_max = w[i].first;
            printf("xlat: thread %d: first call (own X connection) %.2f ms; median %.2f p90 %.2f max %.2f ms, %d above 50 ms\n",
                   i, w[i].first, w[i].med, w[i].p90, w[i].max, w[i].slow);
        }
        printf("xlat: %d threads x %d GetCursorPos: worst round trip %.2f ms, slowest first call %.2f ms, %d above 50 ms\n",
               threads, rounds, worst, first_max, slow);
        fflush(stdout);
        return slow ? 1 : 0;
    }
    double* v = malloc(sizeof(double) * rounds);
    LARGE_INTEGER f, t0, t1;
    QueryPerformanceFrequency(&f);
    POINT p;
    QueryPerformanceCounter(&t0);
    GetCursorPos(&p);
    QueryPerformanceCounter(&t1);
    double first = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)f.QuadPart;
    int slow = 0;
    for (int i = 0; i < rounds; i++) {
        QueryPerformanceCounter(&t0);
        GetCursorPos(&p);
        QueryPerformanceCounter(&t1);
        v[i] = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)f.QuadPart;
        if (v[i] > 50.0) slow++;
        Sleep(3);
    }
    qsort(v, rounds, sizeof(*v), cmp);
    printf("xlat: first call (X connection) %.2f ms; %d GetCursorPos: median %.2f ms, p90 %.2f ms, "
           "max %.2f ms, %d above 50 ms\n", first, rounds, v[rounds / 2], v[rounds * 9 / 10],
           v[rounds - 1], slow);
    fflush(stdout);
    return slow ? 1 : 0;
}
