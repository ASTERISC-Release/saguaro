/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/* The Witcher 1's start-up primitive (FINDINGS.md of proton-vm, 2026-10-17): a thread created
 * suspended, resumed, and waited for with a 100 ms deadline; on timeout the game kills it and
 * hangs. This program does the same ROUNDS times with a thread that returns at once, and reports
 * how long the thread took to finish (median, p90, max) and how many times the deadline expired
 * -- natively against gramine-vm, 32-bit (WoW64) against 64-bit, idle against a loaded host.
 * Usage: threadwait.exe [rounds] [deadline_ms]   (defaults 200 and 100) */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

static DWORD WINAPI noop(LPVOID arg) { (void)arg; return 0; }

static int cmp(const void* a, const void* b) {
    double x = *(const double*)a, y = *(const double*)b;
    return (x > y) - (x < y);
}

int main(int argc, char** argv) {
    int rounds = argc > 1 ? atoi(argv[1]) : 200;
    DWORD deadline = argc > 2 ? (DWORD)atoi(argv[2]) : 100;
    if (rounds <= 0 || rounds > 100000) rounds = 200;
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
    qsort(v, rounds, sizeof(*v), cmp);
    printf("threadwait: %d rounds, deadline %lu ms, %s-bit: resume-to-exit median %.2f ms, p90 %.2f ms, "
           "max %.2f ms, %d timeouts, %d errors\n", rounds, (unsigned long)deadline,
           sizeof(void*) == 8 ? "64" : "32", v[rounds / 2], v[rounds * 9 / 10], v[rounds - 1],
           timeouts, errors);
    fflush(stdout);
    return timeouts ? 1 : 0;
}
