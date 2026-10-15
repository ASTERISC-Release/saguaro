/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/*
 * Regression tests for memfd sealing beyond what the pinned LTP measures: this LTP's
 * memfd_create01 only checks F_ADD_SEALS/F_GET_SEALS bookkeeping, so the enforcement of
 * F_SEAL_SHRINK/GROW/WRITE/FUTURE_WRITE/EXEC is checked here — at write(), ftruncate(), new
 * mmap/mprotect, the existing mapping when a seal is added, and the /proc/self/fd/N reopen
 * (full read-write access by design, refused for write once F_SEAL_FUTURE_WRITE is in place).
 */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <linux/memfd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#ifndef MFD_ALLOW_SEALING
#define MFD_ALLOW_SEALING 0x0002U
#endif
#ifndef F_ADD_SEALS
#define F_ADD_SEALS 1033
#endif
#ifndef F_GET_SEALS
#define F_GET_SEALS 1034
#endif
#ifndef F_SEAL_SEAL
#define F_SEAL_SEAL         0x0001
#define F_SEAL_SHRINK       0x0002
#define F_SEAL_GROW         0x0004
#define F_SEAL_WRITE        0x0008
#define F_SEAL_FUTURE_WRITE 0x0010
#define F_SEAL_EXEC         0x0020
#endif

#define CHECK(cond)                                                \
    do {                                                           \
        if (!(cond)) {                                             \
            fprintf(stderr, "%s:%d: %s failed (errno=%d)\n",       \
                    __FILE__, __LINE__, #cond, errno);             \
            exit(1);                                               \
        }                                                          \
    } while (0)

static int add_seals(int fd, unsigned int seals) {
    return fcntl(fd, F_ADD_SEALS, seals);
}

static unsigned int get_seals(int fd) {
    return (unsigned int)fcntl(fd, F_GET_SEALS);
}

int main(void) {
    int fd = (int)syscall(SYS_memfd_create, "seals", MFD_ALLOW_SEALING);
    CHECK(fd >= 0);
    CHECK(get_seals(fd) == 0);

    CHECK(write(fd, "hello", 5) == 5);
    CHECK(ftruncate(fd, 4096) == 0);

    /* F_SEAL_SHRINK then F_SEAL_GROW pin the size from both sides */
    CHECK(add_seals(fd, F_SEAL_SHRINK) == 0);
    errno = 0;
    CHECK(ftruncate(fd, 100) == -1 && errno == EPERM);
    CHECK(ftruncate(fd, 8192) == 0);
    CHECK(add_seals(fd, F_SEAL_GROW) == 0);
    errno = 0;
    CHECK(ftruncate(fd, 8192 + 4096) == -1 && errno == EPERM);
    CHECK(get_seals(fd) == (F_SEAL_SHRINK | F_SEAL_GROW));

    /* the mapping and write path that the write-seals below restrict against; created after the
     * last size change because the emulated tmpfs mmap reloads mappings from the file there, and
     * the fd write is done before creating the mapping for the same reason: a write to the file
     * reloads (and so discards) shared mappings on the emulated fs, which is fine for the seal
     * checks themselves but must not race with content written through a mapping */
    CHECK(write(fd, "x", 1) == 1);
    char* map = mmap(NULL, 8192, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    CHECK(map != MAP_FAILED);
    map[0] = 'A';

    /* the pre-existing writable mapping keeps working across a FUTURE_WRITE seal, but no new
     * writable mapping or writable reopen appears (the fd predates the seal) */
    CHECK(add_seals(fd, F_SEAL_FUTURE_WRITE) == 0);
    CHECK(mmap(NULL, 8192, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0) == MAP_FAILED);
    /* flush the writable mapping so the read-only one below sees its contents on the emulated
     * tmpfs mmap too (on the host-file backing the page cache shares them already) */
    CHECK(msync(map, 8192, MS_SYNC) == 0);
    char* ro = mmap(NULL, 8192, PROT_READ, MAP_SHARED, fd, 0);
    CHECK(ro != MAP_FAILED);
    CHECK(ro[0] == 'A');
    char path[64];
    snprintf(path, sizeof(path), "/proc/self/fd/%d", fd);
    int rw = open(path, O_RDWR);
    CHECK(rw == -1 && errno == EPERM);
    int rd = open(path, O_RDONLY);
    CHECK(rd >= 0);
    CHECK(write(rd, "x", 1) == -1); /* the reopened fd reads only */
    close(rd);
    /* a private writable mapping does not write through to the file: allowed */
    CHECK(mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0) != MAP_FAILED);

    /* F_SEAL_EXEC refuses executable mappings, new or on an existing range */
    CHECK(add_seals(fd, F_SEAL_EXEC) == 0);
    CHECK((get_seals(fd) & F_SEAL_EXEC) && (get_seals(fd) & F_SEAL_FUTURE_WRITE));
    CHECK(mmap(NULL, 4096, PROT_READ | PROT_EXEC, MAP_PRIVATE, fd, 0) == MAP_FAILED);
    errno = 0;
    CHECK(mprotect(map, 4096, PROT_READ | PROT_WRITE | PROT_EXEC) == -1 && errno == EPERM);
    CHECK(mprotect(ro, 4096, PROT_READ | PROT_WRITE) == -1 && errno == EPERM); /* FUTURE_WRITE */
    /* the already-writable mapping is unaffected by the seals */
    map[1] = 'B';
    CHECK(map[1] == 'B');

    /* F_SEAL_WRITE cannot even be added while a writable shared mapping is live, and refuses
     * writes outright once it is in place */
    errno = 0;
    CHECK(add_seals(fd, F_SEAL_WRITE) == -1 && errno == EBUSY);
    CHECK(munmap(map, 8192) == 0);
    CHECK(add_seals(fd, F_SEAL_WRITE) == 0);
    errno = 0;
    CHECK(write(fd, "x", 1) == -1 && errno == EPERM);
    CHECK(munmap(ro, 4096) == 0);

    /* F_SEAL_SEAL freezes the seal set; a memfd created without MFD_ALLOW_SEALING starts sealed */
    CHECK(add_seals(fd, F_SEAL_SEAL) == 0);
    errno = 0;
    CHECK(add_seals(fd, F_SEAL_SHRINK) == -1 && errno == EPERM);
    CHECK((get_seals(fd) & F_SEAL_SEAL) && (get_seals(fd) & F_SEAL_WRITE));

    int plain = (int)syscall(SYS_memfd_create, "plain", 0);
    CHECK(plain >= 0);
    CHECK(get_seals(plain) == F_SEAL_SEAL);
    errno = 0;
    CHECK(add_seals(plain, F_SEAL_SHRINK) == -1 && errno == EPERM);

    printf("TEST OK\n");
    return 0;
}
