/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 Gramine contributors */

/*
 * Phase-4 attestation test of the VBS-enclave PAL: an application inside the enclave gets an
 * attestation report through Gramine's /dev/attestation interface, with its own user report data,
 * and writes it to the host for the host verifier (`pal_vbs_host.exe <image> 1 verify ...`).
 *
 *   attest <user data as hex, at most 64 bytes> [output directory, default /data/out]
 *
 * Writes <dir>/report.bin (the report), <dir>/quote.bin (the quote: the same package for VBS) and
 * <dir>/user_data.bin (the 64 bytes written to user_report_data).
 */

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int hexval(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static ssize_t read_all(const char* path, void* buf, size_t cap) {
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    size_t got = 0;
    while (got < cap) {
        ssize_t n = read(fd, (char*)buf + got, cap - got);
        if (n < 0) {
            close(fd);
            return -1;
        }
        if (n == 0)
            break;
        got += n;
    }
    close(fd);
    return got;
}

static int write_all(const char* path, const void* buf, size_t len) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return -1;
    ssize_t n = write(fd, buf, len);
    close(fd);
    return n == (ssize_t)len ? 0 : -1;
}

static int fail(const char* what) {
    printf("attest test: FAILED: %s (errno %d)\n", what, errno);
    return 1;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("usage: attest <user data hex> [output directory]\n");
        return 2;
    }
    const char* dir = argc > 2 ? argv[2] : "/data/out";
    uint8_t user_data[64] = {0};
    size_t hl = strlen(argv[1]);
    if (hl % 2 || hl > 128)
        return fail("user data must be at most 64 bytes of hex");
    for (size_t i = 0; i < hl / 2; i++) {
        int hi = hexval(argv[1][2 * i]), lo = hexval(argv[1][2 * i + 1]);
        if (hi < 0 || lo < 0)
            return fail("bad hex");
        user_data[i] = (uint8_t)(hi << 4 | lo);
    }

    char type[32] = {0};
    if (read_all("/dev/attestation/attestation_type", type, sizeof(type) - 1) <= 0)
        return fail("read /dev/attestation/attestation_type");
    printf("attestation_type: %s\n", type);
    if (strcmp(type, "vbs"))
        return fail("attestation type is not \"vbs\"");

    int fd = open("/dev/attestation/user_report_data", O_WRONLY);
    if (fd < 0 || write(fd, user_data, sizeof(user_data)) != sizeof(user_data))
        return fail("write /dev/attestation/user_report_data");
    close(fd);

    static uint8_t report[16384], quote[16384];
    ssize_t rs = read_all("/dev/attestation/report", report, sizeof(report));
    if (rs <= 0)
        return fail("read /dev/attestation/report");
    ssize_t qs = read_all("/dev/attestation/quote", quote, sizeof(quote));
    if (qs <= 0)
        return fail("read /dev/attestation/quote");
    printf("report: %zd bytes, quote: %zd bytes\n", rs, qs);

    /* the package: a 24-byte header, then the report (size, version, then the 64 bytes of data) */
    if (rs < 24 + 8 + 64 || memcmp(report + 24 + 8, user_data, 64))
        return fail("the report does not hold the user data");
    if (qs != rs || memcmp(quote + 24 + 8, user_data, 64))
        return fail("the quote does not hold the user data");
    printf("user data found in the report and the quote\n");

    char path[256];
    snprintf(path, sizeof(path), "%s/report.bin", dir);
    if (write_all(path, report, rs) < 0)
        return fail("write report.bin");
    snprintf(path, sizeof(path), "%s/quote.bin", dir);
    if (write_all(path, quote, qs) < 0)
        return fail("write quote.bin");
    snprintf(path, sizeof(path), "%s/user_data.bin", dir);
    if (write_all(path, user_data, sizeof(user_data)) < 0)
        return fail("write user_data.bin");
    printf("attest test: passed\n");
    return 0;
}
