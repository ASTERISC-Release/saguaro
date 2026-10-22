/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/* The granularity of short timed waits as a Wine program sees them: Sleep(1), Sleep(0),
 * WaitForSingleObject(event, 1 ms) and (event, 5 ms) on a never-signalled event, 50 each;
 * median and max in ms. Under saguaro-vm timed waits are served from the PAL's 100 ms tick
 * (FINDINGS.md of proton-vm, 2026-10-17), so a game that polls with 1 to 5 ms waits runs them
 * at 100 ms each. Usage: sleepgran.exe [rounds]   (default 50) */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

static int cmp(const void* a, const void* b) {
    double x = *(const double*)a, y = *(const double*)b;
    return (x > y) - (x < y);
}

static LARGE_INTEGER freq;

static void report(const char* what, double* v, int n) {
    qsort(v, n, sizeof(*v), cmp);
    printf("sleepgran: %-28s median %7.2f ms, p90 %7.2f ms, max %7.2f ms (%d)\n", what, v[n / 2],
           v[n * 9 / 10], v[n - 1], n);
}

#define TIME(expr, v, i)                                                        \
    do {                                                                        \
        LARGE_INTEGER a, b;                                                     \
        QueryPerformanceCounter(&a);                                            \
        expr;                                                                   \
        QueryPerformanceCounter(&b);                                            \
        v[i] = (double)(b.QuadPart - a.QuadPart) * 1000.0 / (double)freq.QuadPart; \
    } while (0)

int main(int argc, char** argv) {
    int n = argc > 1 ? atoi(argv[1]) : 50;
    if (n <= 0 || n > 10000) n = 50;
    double* v = malloc(sizeof(double) * n);
    QueryPerformanceFrequency(&freq);
    HANDLE ev = CreateEventA(NULL, TRUE, FALSE, NULL);
    for (int i = 0; i < n; i++) TIME(Sleep(0), v, i);
    report("Sleep(0)", v, n);
    for (int i = 0; i < n; i++) TIME(Sleep(1), v, i);
    report("Sleep(1)", v, n);
    for (int i = 0; i < n; i++) TIME(Sleep(5), v, i);
    report("Sleep(5)", v, n);
    for (int i = 0; i < n; i++) TIME(WaitForSingleObject(ev, 1), v, i);
    report("WaitForSingleObject(ev, 1)", v, n);
    for (int i = 0; i < n; i++) TIME(WaitForSingleObject(ev, 5), v, i);
    report("WaitForSingleObject(ev, 5)", v, n);
    for (int i = 0; i < n; i++) TIME(WaitForSingleObject(ev, 20), v, i);
    report("WaitForSingleObject(ev, 20)", v, n);
    fflush(stdout);
    return 0;
}
