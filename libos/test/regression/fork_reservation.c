/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/*
 * fork() with large reserved (PROT_NONE, never written) anonymous memory, as Wine's preloader and
 * ntdll leave it (1.6 GiB at 0x110000, tens of GiB above): the checkpoint must not copy pages that
 * were never writable, and the child must see the memory that was written: a committed piece
 * inside the reservation (mprotect to RW, written), a writable region written in part, a region
 * written and then made read-only, and a region made RW and back to PROT_NONE without a write. The
 * reservation itself stays PROT_NONE in the child. The largest reservation the loader allows is
 * used (16 GiB down to 256 MiB); the fork time is printed for the record.
 */

#define _GNU_SOURCE
#include <err.h>
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "common.h"

static double now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

int main(void) {
    const size_t gib = 1ul << 30;
    size_t reserve = 16 * gib;
    char* res = MAP_FAILED;
    while (reserve >= 256 * (1ul << 20)) {
        res = mmap(NULL, reserve, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        if (res != MAP_FAILED)
            break;
        reserve /= 2;
    }
    if (res == MAP_FAILED)
        err(1, "mmap of the reservation");

    /* a committed piece inside the reservation, written */
    char* committed = res + reserve / 2;
    CHECK(mprotect(committed, 1 << 20, PROT_READ | PROT_WRITE));
    memset(committed, 0x5a, 1 << 20);

    /* a writable region, first half written */
    size_t rw_size = 64 << 20;
    char* rw = mmap(NULL, rw_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (rw == MAP_FAILED)
        err(1, "mmap rw");
    memset(rw, 0xa5, rw_size / 2);

    /* written, then made read-only: the content must survive */
    char* ro = mmap(NULL, 1 << 20, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (ro == MAP_FAILED)
        err(1, "mmap ro");
    memset(ro, 0x3c, 1 << 20);
    CHECK(mprotect(ro, 1 << 20, PROT_READ));

    /* made writable and back to PROT_NONE without a write, then readable in the child: zeros */
    char* back = mmap(NULL, 1 << 20, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (back == MAP_FAILED)
        err(1, "mmap back");
    CHECK(mprotect(back, 1 << 20, PROT_READ | PROT_WRITE));
    CHECK(mprotect(back, 1 << 20, PROT_NONE));

    double t0 = now();
    pid_t pid = CHECK(fork());
    if (pid == 0) {
        for (size_t i = 0; i < (1 << 20); i += 4096)
            if (committed[i] != 0x5a)
                errx(1, "child: committed piece lost at %zu", i);
        for (size_t i = 0; i < rw_size / 2; i += 4096)
            if (rw[i] != (char)0xa5)
                errx(1, "child: written half lost at %zu", i);
        for (size_t i = rw_size / 2; i < rw_size; i += 4096)
            if (rw[i] != 0)
                errx(1, "child: unwritten half not zero at %zu", i);
        for (size_t i = 0; i < (1 << 20); i += 4096)
            if (ro[i] != 0x3c)
                errx(1, "child: read-only region lost at %zu", i);
        CHECK(mprotect(back, 1 << 20, PROT_READ));
        for (size_t i = 0; i < (1 << 20); i += 4096)
            if (back[i] != 0)
                errx(1, "child: PROT_NONE region not zero at %zu", i);
        /* the reservation is still PROT_NONE: a read must fault */
        pid_t probe = CHECK(fork());
        if (probe == 0) {
            volatile char c = res[4096];
            (void)c;
            _exit(0);
        }
        int status = 0;
        CHECK(waitpid(probe, &status, 0));
        if (!WIFSIGNALED(status) || WTERMSIG(status) != SIGSEGV)
            errx(1, "child: the reservation is readable in the grandchild (status %#x)", status);
        /* and it can still be committed */
        CHECK(mprotect(res + 4096, 4096, PROT_READ | PROT_WRITE));
        res[4096] = 1;
        return 0;
    }
    int status = 0;
    CHECK(waitpid(pid, &status, 0));
    double dt = now() - t0;
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        errx(1, "child failed: status %#x", status);
    printf("reservation %zu MiB, fork and child done in %.3f s\n", reserve >> 20, dt);
    puts("TEST OK");
    return 0;
}
