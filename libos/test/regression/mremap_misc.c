/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/*
 * mremap() (LTP mremap01-05): shrinking in place, growing in place when the pages behind are
 * free and ENOMEM without MREMAP_MAYMOVE when they are not, moving with MREMAP_MAYMOVE and to a
 * given address with MREMAP_FIXED, a file mapping keeping its file contents, MREMAP_DONTUNMAP,
 * EFAULT for an unmapped range and EINVAL for bad flags or an unaligned address. Also the
 * unmapped-range answers of mprotect() and mlock()/munlock() (ENOMEM; LTP mprotect01,
 * munlock02).
 *
 * Usage: mremap_misc <scratch directory>
 */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#ifndef MREMAP_DONTUNMAP
#define MREMAP_DONTUNMAP 4
#endif

#define CHECK(cond)                                                \
    do {                                                           \
        if (!(cond)) {                                             \
            fprintf(stderr, "%s:%d: %s failed (errno=%d)\n",       \
                    __FILE__, __LINE__, #cond, errno);             \
            exit(1);                                               \
        }                                                          \
    } while (0)

#define PG 4096

static char* map(size_t len) {
    char* p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(p != MAP_FAILED);
    return p;
}

int main(int argc, char** argv) {
    if (argc != 2)
        return 1;

    /* a reserved area: 8 pages, of which we use the first ones */
    char* area = map(8 * PG);
    CHECK(munmap(area + 2 * PG, 6 * PG) == 0);
    memset(area, 'a', 2 * PG);

    /* grow in place: the pages behind are free */
    char* p = mremap(area, 2 * PG, 4 * PG, 0);
    CHECK(p == area && p[PG] == 'a' && p[3 * PG] == 0);
    p[3 * PG] = 'b';
    /* shrink in place */
    CHECK(mremap(area, 4 * PG, PG, 0) == area);
    CHECK(area[0] == 'a');

    /* blocked by a neighbour: ENOMEM without MAYMOVE, a move with it */
    char* wall = mmap(area + PG, PG, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    CHECK(wall == area + PG);
    CHECK(mremap(area, PG, 2 * PG, 0) == MAP_FAILED && errno == ENOMEM);
    char* moved = mremap(area, PG, 3 * PG, MREMAP_MAYMOVE);
    CHECK(moved != MAP_FAILED && moved != area && moved[0] == 'a' && moved[2 * PG] == 0);
    CHECK(munmap(wall, PG) == 0);

    /* MREMAP_FIXED: to a given address, the old range goes away */
    char* dest = map(3 * PG);
    CHECK(mremap(moved, 3 * PG, 3 * PG, MREMAP_MAYMOVE | MREMAP_FIXED, dest) == dest);
    CHECK(dest[0] == 'a');
    CHECK(mremap(dest, PG, PG, MREMAP_FIXED, moved) == MAP_FAILED && errno == EINVAL);
    CHECK(mremap(dest, 3 * PG, 3 * PG, MREMAP_MAYMOVE | MREMAP_FIXED, dest + PG) == MAP_FAILED &&
          errno == EINVAL); /* overlap */

    /* errors */
    CHECK(mremap(dest + 1, PG, 2 * PG, MREMAP_MAYMOVE) == MAP_FAILED && errno == EINVAL);
    CHECK(mremap(dest, PG, 2 * PG, 0x100) == MAP_FAILED && errno == EINVAL);
    CHECK(mremap(dest, PG, 0, MREMAP_MAYMOVE) == MAP_FAILED && errno == EINVAL);
    char* hole = map(PG);
    CHECK(munmap(hole, PG) == 0);
    CHECK(mremap(hole, PG, 2 * PG, MREMAP_MAYMOVE) == MAP_FAILED && errno == EFAULT);

    /* a range reaching from one mapping into its neighbour is not one mapping: EFAULT, and the
     * neighbour keeps its contents and protection (LTP mremap03's PROT_NONE "bad address") */
    char* pair = map(2 * PG);
    pair[PG] = 'n';
    CHECK(mprotect(pair, PG, PROT_NONE) == 0);
    CHECK(mremap(pair, 2 * PG, 4 * PG, MREMAP_MAYMOVE) == MAP_FAILED && errno == EFAULT);
    CHECK(pair[PG] == 'n');
    pair[PG] = 'm'; /* still writable */
    CHECK(munmap(pair, 2 * PG) == 0);

    /* MREMAP_DONTUNMAP keeps the old range, emptied */
    char* src = map(PG);
    src[0] = 'z';
    char* copy = mremap(src, PG, PG, MREMAP_MAYMOVE | MREMAP_DONTUNMAP);
    if (copy != MAP_FAILED) { /* older kernels lack it; under the LibOS it exists */
        CHECK(copy != src && copy[0] == 'z' && src[0] == 0);
        CHECK(munmap(copy, PG) == 0);
    }
    CHECK(munmap(src, PG) == 0);
    CHECK(munmap(dest, 3 * PG) == 0);

    /* a shared file mapping grows with the file's contents */
    char path[512];
    snprintf(path, sizeof(path), "%s/mremap_misc_file", argv[1]);
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    CHECK(fd >= 0);
    CHECK(ftruncate(fd, 4 * PG) == 0);
    CHECK(pwrite(fd, "tail", 4, 3 * PG) == 4);
    char* f = mmap(NULL, PG, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    CHECK(f != MAP_FAILED);
    f[0] = 'h';
    char* g = mremap(f, PG, 4 * PG, MREMAP_MAYMOVE);
    CHECK(g != MAP_FAILED && g[0] == 'h' && memcmp(g + 3 * PG, "tail", 4) == 0);
    CHECK(munmap(g, 4 * PG) == 0);
    close(fd);
    unlink(path);

    /* unmapped ranges */
    char* gone = map(2 * PG);
    CHECK(munmap(gone, 2 * PG) == 0);
    CHECK(mprotect(NULL, PG + 1, PROT_READ) == -1 && errno == ENOMEM);
    CHECK(mprotect(gone, PG, PROT_READ) == -1 && errno == ENOMEM);
    CHECK(mprotect(gone + 1, PG, PROT_READ) == -1 && errno == EINVAL);
    CHECK(munlock(gone, PG) == -1 && errno == ENOMEM);
    char* here = map(PG);
    CHECK(mlock(here, PG) == 0 && munlock(here, PG) == 0);
    CHECK(munmap(here, PG) == 0);

    puts("TEST OK");
    return 0;
}
