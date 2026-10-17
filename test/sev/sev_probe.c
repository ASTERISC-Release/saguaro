/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/*
 * sev_probe: the guest side of test/test-sev.sh (Documentation/devel/sev/).
 *
 * 1. Reads the file named by argv[1] (through virtiofs, i.e. the bounce buffers of the shared
 *    region) and prints its length and a checksum, so the host can compare.
 * 2. Fills a buffer with a random marker that never leaves the guest, prints the buffer's address
 *    (the vm PAL maps a single process 1:1, so it is the guest-physical address) and the marker
 *    XOR-ed with 0x5a (the console is host-visible; the marker itself must not appear there), then
 *    prints "READY" and waits until the host creates the file argv[2] (again through virtiofs),
 *    while the host dumps guest memory and looks for the marker.
 */

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <unistd.h>

#define BUF_SIZE (1024 * 1024)
#define MARKER_LEN 32

static char g_buf[BUF_SIZE] __attribute__((aligned(4096)));

int main(int argc, char** argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s <input file> <done file>\n", argv[0]);
        return 2;
    }

    int fd = open(argv[1], O_RDONLY);
    if (fd < 0) {
        perror("open input");
        return 1;
    }
    uint64_t sum = 0, len = 0;
    for (;;) {
        ssize_t n = read(fd, g_buf, sizeof(g_buf));
        if (n < 0) {
            perror("read input");
            return 1;
        }
        if (n == 0)
            break;
        for (ssize_t i = 0; i < n; i++)
            sum = sum * 31 + (unsigned char)g_buf[i];
        len += n;
    }
    close(fd);
    printf("INPUT len=%lu sum=%016lx\n", (unsigned long)len, (unsigned long)sum);

    unsigned char rnd[MARKER_LEN / 2];
    if (getrandom(rnd, sizeof(rnd), 0) != sizeof(rnd)) {
        perror("getrandom");
        return 1;
    }
    char marker[MARKER_LEN + 1];
    for (size_t i = 0; i < sizeof(rnd); i++)
        snprintf(&marker[2 * i], 3, "%02x", rnd[i]);
    for (size_t off = 0; off + MARKER_LEN <= BUF_SIZE; off += MARKER_LEN)
        memcpy(&g_buf[off], marker, MARKER_LEN);

    char hidden[2 * MARKER_LEN + 1];
    for (size_t i = 0; i < MARKER_LEN; i++)
        snprintf(&hidden[2 * i], 3, "%02x", (unsigned char)(marker[i] ^ 0x5a));
    printf("SECRET addr=%p size=%d xor5a=%s\n", (void*)g_buf, BUF_SIZE, hidden);
    printf("READY\n");
    fflush(stdout);

    for (int i = 0; i < 600; i++) {
        if (access(argv[2], F_OK) == 0) {
            /* use the buffer, so that it stays live until here */
            printf("DONE %c\n", g_buf[BUF_SIZE - 1]);
            return 0;
        }
        usleep(100 * 1000);
    }
    fprintf(stderr, "timed out waiting for %s\n", argv[2]);
    return 1;
}
