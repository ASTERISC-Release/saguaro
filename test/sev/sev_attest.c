/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/*
 * sev_attest: the guest side of `test/test-sev.sh attest` (Documentation/devel/sev/). Writes the
 * 64 bytes given as 128 hex digits in argv[1] to /dev/attestation/user_report_data, reads
 * /dev/attestation/quote (under SEV-SNP the firmware-signed attestation report) and writes it to
 * the file argv[2] (on the host, through virtiofs), which the host then verifies.
 */

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int read_file(const char* path, char* buf, size_t size, size_t* out) {
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    size_t got = 0;
    for (;;) {
        ssize_t n = read(fd, buf + got, size - got);
        if (n < 0) {
            close(fd);
            return -1;
        }
        if (n == 0)
            break;
        got += n;
    }
    close(fd);
    *out = got;
    return 0;
}

int main(int argc, char** argv) {
    if (argc != 3 || strlen(argv[1]) != 128) {
        fprintf(stderr, "usage: %s <128 hex digits of report data> <output file>\n", argv[0]);
        return 2;
    }
    unsigned char data[64];
    for (int i = 0; i < 64; i++) {
        unsigned v;
        if (sscanf(argv[1] + 2 * i, "%2x", &v) != 1)
            return 2;
        data[i] = (unsigned char)v;
    }

    char type[64] = {0};
    size_t n;
    if (read_file("/dev/attestation/attestation_type", type, sizeof(type) - 1, &n) < 0) {
        perror("/dev/attestation/attestation_type");
        return 1;
    }
    printf("attestation_type=%s\n", type);

    int fd = open("/dev/attestation/user_report_data", O_WRONLY);
    if (fd < 0 || write(fd, data, sizeof(data)) != sizeof(data)) {
        perror("/dev/attestation/user_report_data");
        return 1;
    }
    close(fd);

    static char quote[8192];
    if (read_file("/dev/attestation/quote", quote, sizeof(quote), &n) < 0) {
        perror("/dev/attestation/quote");
        return 1;
    }
    printf("quote size=%zu\n", n);

    fd = open(argv[2], O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0 || write(fd, quote, n) != (ssize_t)n) {
        perror(argv[2]);
        return 1;
    }
    close(fd);
    printf("REPORT WRITTEN\n");
    return 0;
}
