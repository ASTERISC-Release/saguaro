/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/*
 * A memfd must support read(), write() and mmap() even when /dev/shm is an `untrusted_shm`
 * mount (gramine-direct's default for LTP), which maps but cannot read or write: memfd_create
 * then backs the memfd elsewhere (LTP memfd_create01 read EBADF before).
 */

#define _GNU_SOURCE

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#define CHECK(cond)                                                \
    do {                                                           \
        if (!(cond)) {                                             \
            fprintf(stderr, "%s:%d: %s failed (errno=%d)\n",       \
                    __FILE__, __LINE__, #cond, errno);             \
            exit(1);                                               \
        }                                                          \
    } while (0)

int main(void) {
    int fd = (int)syscall(SYS_memfd_create, "rw", 0);
    CHECK(fd >= 0);
    CHECK(write(fd, "hello memfd", 11) == 11);
    char buf[16] = {0};
    CHECK(pread(fd, buf, sizeof(buf), 0) == 11 && memcmp(buf, "hello memfd", 11) == 0);
    CHECK(lseek(fd, 0, SEEK_SET) == 0);
    CHECK(read(fd, buf, 5) == 5 && memcmp(buf, "hello", 5) == 0);
    CHECK(ftruncate(fd, 4096) == 0);
    char* p = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    CHECK(p != MAP_FAILED);
    CHECK(memcmp(p, "hello memfd", 11) == 0);
    CHECK(munmap(p, 4096) == 0);
    close(fd);
    puts("TEST OK");
    return 0;
}
