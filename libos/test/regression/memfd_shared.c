/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/*
 * memfd used the way Mesa's Vulkan drivers use it (Documentation/devel/vulkan/): a large sparse
 * size set with ftruncate, a MAP_SHARED mapping at the start, another one far into the file,
 * zero-filled unwritten pages, pwrite() seen by the mappings, and munmap/close.
 *
 * Two things Mesa also relies on are not checked, because the generic path (a file on the tmpfs
 * mount of /dev/shm) does not provide them in Gramine: two MAP_SHARED mappings of the same pages
 * seeing each other's stores, and read() seeing a store made through a mapping (Gramine maps tmpfs
 * files as copies). Under gramine-vm with the graphics redirection active, memfd_create returns a
 * host-backed file whose mappings are real host mappings; that path is exercised by test/vulkan
 * in a VM with the host listener, since this suite has no listener.
 */

#define _GNU_SOURCE

#include <err.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001U
#endif
#ifndef MFD_ALLOW_SEALING
#define MFD_ALLOW_SEALING 0x0002U
#endif

#define FILE_SIZE (64UL << 20)
#define FAR_OFFSET (48UL << 20)
#define FAR_SIZE (2UL << 20)

int main(void) {
    int fd = (int)syscall(SYS_memfd_create, "memfd_shared", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd < 0)
        err(1, "memfd_create");
    if (ftruncate(fd, FILE_SIZE) < 0)
        err(1, "ftruncate");
    if (lseek(fd, 0, SEEK_END) != (off_t)FILE_SIZE)
        errx(1, "size after ftruncate is not %lu", FILE_SIZE);

    uint32_t word = 0x12345678;
    if (pwrite(fd, &word, sizeof(word), 64) != (ssize_t)sizeof(word))
        err(1, "pwrite");

    uint32_t* a = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (a == MAP_FAILED)
        err(1, "mmap");
    if (a[16] != 0x12345678)
        errx(1, "the mapping does not see the earlier pwrite");
    if (a[0] != 0 || a[1023] != 0)
        errx(1, "unwritten part of the first page is not zero");
    for (uint32_t i = 0; i < 1024; i++)
        a[i] = i * 7 + 1;
    for (uint32_t i = 0; i < 1024; i++)
        if (a[i] != i * 7 + 1)
            errx(1, "store through the mapping lost at %u", i);

    uint8_t* far = mmap(NULL, FAR_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, FAR_OFFSET);
    if (far == MAP_FAILED)
        err(1, "mmap far");
    if (far[0] != 0 || far[FAR_SIZE - 1] != 0)
        errx(1, "unwritten part of the file is not zero");
    memset(far, 0x5a, FAR_SIZE);
    if (far[FAR_SIZE / 2] != 0x5a)
        errx(1, "store through the far mapping lost");

    if (munmap(a, 4096) < 0 || munmap(far, FAR_SIZE) < 0)
        err(1, "munmap");
    if (close(fd) < 0)
        err(1, "close");
    puts("TEST OK");
    return 0;
}
