/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2020 Intel Corporation
 *                    Michał Kowalczyk <mkow@invisiblethingslab.com>
 */
#define _GNU_SOURCE
#include <err.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>

/* Unfortunately LTP tests for madvise require a lot of other complex features (e.g. mount or
 * cgroup), so let's test it by ourselves. */

#define PAGES_CNT 128

static void test_madvise_rw(void) {
    size_t page_size = getpagesize();

    char* m = (char*)mmap(NULL, PAGES_CNT * page_size,
                          PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS,
                          -1, 0);
    if (m == MAP_FAILED)
        err(1, "mmap()");

    for (size_t i = 0; i < PAGES_CNT; i++) {
        *(int*)(m + page_size * i) = 0x123;
        *(int*)(m + page_size * i + page_size - sizeof(int)) = 0x321;
    }

    /* Clear pages one by one */
    bool cleared[PAGES_CNT] = {false};
    for (size_t i = 0; i < PAGES_CNT; i++) {
        size_t perm_idx = ((i + 123) * 51) % PAGES_CNT; /* make the traverse non-sequential */
        char* addr = m + page_size * perm_idx;
        size_t size = page_size - (i * 31337 % page_size); /* test aligning up of the size */

        int res = madvise(addr, size, MADV_DONTNEED);
        if (res)
            err(1, "madvise(%p, 0x%zx, MADV_DONTNEED) failed", addr, size);
        cleared[perm_idx] = true;

        /* Rescan the whole range and verify all magic values */
        for (size_t j = 0; j < PAGES_CNT; j++) {
            int expected1 = cleared[j] ? 0 : 0x123;
            int expected2 = cleared[j] ? 0 : 0x321;
            int actual1 = *(int*)(m + page_size * j);
            int actual2 = *(int*)(m + page_size * j + page_size - sizeof(int));
            if (actual1 != expected1 || actual2 != expected2) {
                errx(1, "page %zu has wrong contents: 0x%x != 0x%x or 0x%x != 0x%x", j, actual1,
                     expected1, actual2, expected2);
            }
        }
    }

    if (munmap(m, PAGES_CNT * page_size) < 0)
        err(1, "munmap");
}

/* this test emulates Node.js v20 behavior */
static void test_madvise_none(void) {
    size_t page_size = getpagesize();

    char* m = (char*)mmap(NULL, PAGES_CNT * page_size,
                          PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS,
                          -1, 0);
    if (m == MAP_FAILED)
        err(1, "mmap()");

    int res = madvise(m, PAGES_CNT * page_size, MADV_DONTNEED);
    if (res)
        err(1, "madvise(%p, 0x%zx, MADV_DONTNEED) failed", m, PAGES_CNT * page_size);

    if (munmap(m, PAGES_CNT * page_size) < 0)
        err(1, "munmap");
}

/* a large mapping of which few pages were touched (what glibc does to the stack of every exiting
 * thread): the touched pages, one of them PROT_NONE meanwhile, read as zeros afterwards and the
 * range is usable again */
static void test_madvise_sparse(void) {
    size_t page_size = getpagesize();
    size_t pages = 8192; /* 32 MiB */

    char* m = (char*)mmap(NULL, pages * page_size, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (m == MAP_FAILED)
        err(1, "mmap()");

    size_t touched[] = {0, 1, 100, 4096, pages - 1};
    for (size_t i = 0; i < sizeof(touched) / sizeof(touched[0]); i++)
        *(int*)(m + page_size * touched[i]) = 0x456;
    if (mprotect(m + page_size * 100, page_size, PROT_NONE) < 0)
        err(1, "mprotect(PROT_NONE)");

    if (madvise(m, pages * page_size, MADV_DONTNEED))
        err(1, "madvise(MADV_DONTNEED)");

    if (mprotect(m + page_size * 100, page_size, PROT_READ | PROT_WRITE) < 0)
        err(1, "mprotect(PROT_READ | PROT_WRITE)");
    for (size_t i = 0; i < pages; i += 7) {
        if (*(int*)(m + page_size * i) != 0)
            errx(1, "page %zu is not zero after MADV_DONTNEED", i);
    }
    for (size_t i = 0; i < sizeof(touched) / sizeof(touched[0]); i++) {
        if (*(int*)(m + page_size * touched[i]) != 0)
            errx(1, "touched page %zu is not zero after MADV_DONTNEED", touched[i]);
        *(int*)(m + page_size * touched[i]) = 0x789;
        if (*(int*)(m + page_size * touched[i]) != 0x789)
            errx(1, "touched page %zu cannot be written again", touched[i]);
    }

    if (munmap(m, pages * page_size) < 0)
        err(1, "munmap");
}

int main(void) {
    test_madvise_rw();
    test_madvise_none();
    test_madvise_sparse();
    puts("TEST OK");
    return 0;
}
