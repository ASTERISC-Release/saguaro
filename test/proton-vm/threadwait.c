/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/* The Witcher 1's start-up primitive (FINDINGS.md of proton-vm, 2026-10-17): a thread created
 * suspended, resumed, and waited for with a 100 ms deadline; on timeout the game kills it and
 * hangs. This program does the same ROUNDS times with a thread that returns at once, and reports
 * how long the thread took to finish (median, p90, max) and how many times the deadline expired
 * -- natively against gramine-vm, 32-bit (WoW64) against 64-bit, idle against a loaded host.
 * Usage: threadwait.exe [rounds] [deadline_ms] [pollers]   (defaults 200, 100 and 0: with N > 0,
 * N sibling threads poll an event every 5 ms meanwhile, as the game's do) */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

static DWORD WINAPI noop(LPVOID arg) { (void)arg; return 0; }

/* The game's sibling: a thread that waits on a never-signalled event with a 5 ms timeout in a loop
 * (its thread 014c does select(WAIT event, +0.005) / release_mutex without pause). */
static volatile int poller_stop;
static DWORD WINAPI poller(LPVOID arg) {
    HANDLE ev = (HANDLE)arg;
    while (!poller_stop)
        WaitForSingleObject(ev, 5);
    return 0;
}

static int cmp(const void* a, const void* b) {
    double x = *(const double*)a, y = *(const double*)b;
    return (x > y) - (x < y);
}

int main(int argc, char** argv) {
    int rounds = argc > 1 ? atoi(argv[1]) : 200;
    DWORD deadline = argc > 2 ? (DWORD)atoi(argv[2]) : 100;
    int pollers = argc > 3 ? atoi(argv[3]) : 0;
    if (rounds <= 0 || rounds > 100000) rounds = 200;
    HANDLE ev = CreateEventA(NULL, TRUE, FALSE, NULL);
    HANDLE pt[16];
    if (pollers > 16) pollers = 16;
    for (int i = 0; i < pollers; i++)
        pt[i] = CreateThread(NULL, 0, poller, ev, 0, NULL);
    double* v = malloc(sizeof(double) * rounds);
    LARGE_INTEGER f, t0, t1;
    QueryPerformanceFrequency(&f);
    int timeouts = 0, errors = 0;
    for (int i = 0; i < rounds; i++) {
        HANDLE h = CreateThread(NULL, 0, noop, NULL, CREATE_SUSPENDED, NULL);
        if (!h) { errors++; v[i] = -1; continue; }
        QueryPerformanceCounter(&t0);
        ResumeThread(h);
        DWORD r = WaitForSingleObject(h, deadline);
        QueryPerformanceCounter(&t1);
        v[i] = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)f.QuadPart;
        if (r == WAIT_TIMEOUT) {
            timeouts++;
            WaitForSingleObject(h, INFINITE); /* let it finish instead of killing it */
        } else if (r != WAIT_OBJECT_0) {
            errors++;
        }
        CloseHandle(h);
        Sleep(3); /* let the CPUs go idle between rounds, as between a game's steps */
    }
    poller_stop = 1;
    for (int i = 0; i < pollers; i++) { WaitForSingleObject(pt[i], INFINITE); CloseHandle(pt[i]); }
    qsort(v, rounds, sizeof(*v), cmp);
    printf("threadwait: %d rounds, deadline %lu ms, %d pollers, %s-bit: resume-to-exit median %.2f ms, p90 %.2f ms, "
           "max %.2f ms, %d timeouts, %d errors\n", rounds, (unsigned long)deadline, pollers,
           sizeof(void*) == 8 ? "64" : "32", v[rounds / 2], v[rounds * 9 / 10], v[rounds - 1],
           timeouts, errors);
    fflush(stdout);
    return timeouts ? 1 : 0;
}
