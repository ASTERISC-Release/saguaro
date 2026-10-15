/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/*
 * mremap() without MREMAP_MAYMOVE: growing in place must fail with ENOMEM when the pages after the
 * mapping are in use, and must not touch them (LuaJIT's allocator relies on this); when they are
 * free it grows in place and keeps the data. Also MREMAP_MAYMOVE growth keeps the data.
 */

#define _GNU_SOURCE
#include <err.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

int main(void) {
    size_t pg = (size_t)sysconf(_SC_PAGESIZE);

    /* reserve 4 pages, then keep [0, 2) as A and [2, 4) as B */
    char* a = mmap(NULL, 4 * pg, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (a == MAP_FAILED)
        err(1, "mmap");
    char* b = a + 2 * pg;
    memset(a, 'A', 2 * pg);
    memset(b, 'B', 2 * pg);
    /* split into two mappings with different protections so that they are separate VMAs */
    if (mprotect(b, 2 * pg, PROT_READ) < 0)
        err(1, "mprotect");

    errno = 0;
    void* r = mremap(a, 2 * pg, 4 * pg, 0);
    if (r != MAP_FAILED || errno != ENOMEM)
        errx(1, "growing in place over a used range: got %p errno %d, expected ENOMEM", r, errno);
    for (size_t i = 0; i < 2 * pg; i++)
        if (b[i] != 'B' || a[i] != 'A')
            errx(1, "data changed by the failed mremap at offset %zu", i);

    if (munmap(b, 2 * pg) < 0)
        err(1, "munmap");
    r = mremap(a, 2 * pg, 4 * pg, 0);
    if (r != a)
        err(1, "growing in place over a free range: got %p, expected %p", r, (void*)a);
    for (size_t i = 0; i < 2 * pg; i++)
        if (a[i] != 'A')
            errx(1, "data lost by growing in place at offset %zu", i);
    for (size_t i = 2 * pg; i < 4 * pg; i++)
        if (a[i] != 0)
            errx(1, "grown part not zero at offset %zu", i);

    char* m = mremap(a, 4 * pg, 8 * pg, MREMAP_MAYMOVE);
    if (m == MAP_FAILED)
        err(1, "mremap MREMAP_MAYMOVE");
    for (size_t i = 0; i < 2 * pg; i++)
        if (m[i] != 'A')
            errx(1, "data lost by MREMAP_MAYMOVE at offset %zu", i);
    munmap(m, 8 * pg);

    puts("TEST OK");
    return 0;
}
