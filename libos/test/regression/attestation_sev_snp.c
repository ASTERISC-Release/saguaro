/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/*
 * /dev/attestation under AMD SEV-SNP (the SEV build of the vm PAL, Documentation/devel/sev/):
 * attestation_type is "sev-snp", `report` and `quote` are SNP attestation reports (1184 bytes)
 * that carry the 64 bytes written to `user_report_data`, the files can be opened and read many
 * times (no resource leak), and two reads with different report data give different reports.
 * The signature is checked on the host (test/test-sev.sh attest), not here.
 */

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define REPORT_SIZE 1184

static ssize_t read_all(const char* path, void* buf, size_t size) {
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    size_t got = 0;
    for (;;) {
        ssize_t n = read(fd, (char*)buf + got, size - got);
        if (n < 0) {
            close(fd);
            return -1;
        }
        if (n == 0)
            break;
        got += n;
        if (got == size)
            break;
    }
    close(fd);
    return got;
}

static int write_all(const char* path, const void* buf, size_t size) {
    int fd = open(path, O_WRONLY);
    if (fd < 0)
        return -1;
    ssize_t n = write(fd, buf, size);
    close(fd);
    return n == (ssize_t)size ? 0 : -1;
}

static int check_report(const char* path, const uint8_t data[64], uint8_t out[REPORT_SIZE]) {
    if (write_all("/dev/attestation/user_report_data", data, 64) < 0) {
        perror("user_report_data");
        return -1;
    }
    ssize_t n = read_all(path, out, REPORT_SIZE);
    if (n != REPORT_SIZE) {
        fprintf(stderr, "%s: read %zd bytes, expected %d\n", path, n, REPORT_SIZE);
        return -1;
    }
    uint32_t version, vmpl, sig_algo;
    memcpy(&version, out + 0x0, 4);
    memcpy(&vmpl, out + 0x30, 4);
    memcpy(&sig_algo, out + 0x34, 4);
    if (version < 2 || version > 5 || vmpl != 0 || sig_algo != 1) {
        fprintf(stderr, "%s: version %u, vmpl %u, signature algorithm %u\n", path, version, vmpl,
                sig_algo);
        return -1;
    }
    if (memcmp(out + 0x50, data, 64)) {
        fprintf(stderr, "%s: REPORT_DATA is not the user_report_data\n", path);
        return -1;
    }
    return 0;
}

int main(void) {
    char type[32] = {0};
    if (read_all("/dev/attestation/attestation_type", type, sizeof(type) - 1) < 0 ||
            strcmp(type, "sev-snp")) {
        fprintf(stderr, "attestation_type is '%s', expected 'sev-snp'\n", type);
        return 1;
    }
    printf("attestation_type: %s\n", type);

    static uint8_t a[REPORT_SIZE], b[REPORT_SIZE];
    uint8_t data[64];
    for (int i = 0; i < 64; i++)
        data[i] = (uint8_t)i;

    for (int i = 0; i < 100; i++) {
        data[0] = (uint8_t)i;
        if (check_report(i % 2 ? "/dev/attestation/quote" : "/dev/attestation/report", data, a))
            return 1;
    }
    printf("Test resource leaks in attestation filesystem... SUCCESS\n");

    data[0] = 0xaa;
    if (check_report("/dev/attestation/report", data, a))
        return 1;
    data[0] = 0xbb;
    if (check_report("/dev/attestation/quote", data, b))
        return 1;
    /* same launch: same measurement (0x90..0xc0), different report data */
    if (memcmp(a + 0x90, b + 0x90, 48) || !memcmp(a + 0x50, b + 0x50, 64)) {
        fprintf(stderr, "two reports of one guest differ in their measurement\n");
        return 1;
    }
    printf("Test SEV-SNP report interface... SUCCESS\n");
    return 0;
}
