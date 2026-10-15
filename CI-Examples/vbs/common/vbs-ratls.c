/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/* vbs-ratls: see vbs-ratls.h. */

#include "vbs-ratls.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "mbedtls/ecp.h"
#include "mbedtls/sha256.h"
#include "mbedtls/x509_crt.h"

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static int gen_key(mbedtls_pk_context* pk, int (*f_rng)(void*, unsigned char*, size_t),
                   void* p_rng) {
    mbedtls_pk_init(pk);
    int ret = mbedtls_pk_setup(pk, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY));
    if (ret)
        return ret;
    return mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(*pk), f_rng, p_rng);
}

int vbs_ratls_spki_sha256(mbedtls_pk_context* pk, unsigned char out[32]) {
    unsigned char buf[512];
    int len = mbedtls_pk_write_pubkey_der(pk, buf, sizeof(buf));
    if (len < 0)
        return len;
    /* the DER is written at the end of the buffer */
    return mbedtls_sha256(buf + sizeof(buf) - len, (size_t)len, out, 0);
}

/* the VBS report whose user data is SHA-256(SPKI of `pk`) || zeros */
static int get_report(mbedtls_pk_context* pk, unsigned char* report, size_t* report_len) {
    unsigned char user_data[64] = {0};
    int ret = vbs_ratls_spki_sha256(pk, user_data);
    if (ret)
        return ret;
    int fd = open("/dev/attestation/user_report_data", O_WRONLY);
    if (fd < 0)
        return -errno;
    ssize_t n = write(fd, user_data, sizeof(user_data));
    close(fd);
    if (n != (ssize_t)sizeof(user_data))
        return -EIO;
    fd = open("/dev/attestation/report", O_RDONLY);
    if (fd < 0)
        return -errno;
    size_t got = 0;
    while (got < VBS_RATLS_REPORT_MAX) {
        n = read(fd, report + got, VBS_RATLS_REPORT_MAX - got);
        if (n <= 0)
            break;
        got += (size_t)n;
    }
    close(fd);
    if (got <= VBS_RATLS_USER_DATA_OFFSET + 64)
        return -EIO;
    *report_len = got;
    return 0;
}

/* a self-signed certificate for `pk` with the report as an extension (an OCTET STRING) */
static int make_cert(mbedtls_pk_context* pk, const unsigned char* report, size_t report_len,
                     int (*f_rng)(void*, unsigned char*, size_t), void* p_rng,
                     mbedtls_x509_crt* crt, const char** what) {
    static const char name[] = "CN=gramine-vbs-ratls,O=VBS enclave";
    mbedtls_x509write_cert w;
    unsigned char* val = NULL;
    unsigned char* der = NULL;
    size_t der_cap = VBS_RATLS_REPORT_MAX + 4096;
    int ret;
    mbedtls_x509write_crt_init(&w);
    mbedtls_x509write_crt_set_version(&w, MBEDTLS_X509_CRT_VERSION_3);
    mbedtls_x509write_crt_set_md_alg(&w, MBEDTLS_MD_SHA256);
    mbedtls_x509write_crt_set_subject_key(&w, pk);
    mbedtls_x509write_crt_set_issuer_key(&w, pk);
    *what = "certificate names";
    if ((ret = mbedtls_x509write_crt_set_subject_name(&w, name)) ||
            (ret = mbedtls_x509write_crt_set_issuer_name(&w, name)))
        goto out;
    unsigned char serial[8];
    *what = "certificate serial";
    if ((ret = f_rng(p_rng, serial, sizeof(serial))))
        goto out;
    serial[0] &= 0x7f;
    if ((ret = mbedtls_x509write_crt_set_serial_raw(&w, serial, sizeof(serial))))
        goto out;
    /* the enclave's clock is the host's: the validity period is informational only */
    *what = "certificate validity";
    if ((ret = mbedtls_x509write_crt_set_validity(&w, "20250101000000", "20351231235959")))
        goto out;
    if ((ret = mbedtls_x509write_crt_set_basic_constraints(&w, 0, -1)))
        goto out;
    /* OCTET STRING { report } */
    *what = "report extension";
    val = malloc(4 + report_len);
    der = malloc(der_cap);
    if (!val || !der) {
        ret = -ENOMEM;
        goto out;
    }
    val[0] = 0x04;
    val[1] = 0x82;
    val[2] = (unsigned char)(report_len >> 8);
    val[3] = (unsigned char)report_len;
    memcpy(val + 4, report, report_len);
    if ((ret = mbedtls_x509write_crt_set_extension(&w, VBS_RATLS_OID_DER, VBS_RATLS_OID_LEN, 0, val,
                                                   4 + report_len)))
        goto out;
    *what = "certificate";
    int len = mbedtls_x509write_crt_der(&w, der, der_cap, f_rng, p_rng);
    if (len < 0) {
        ret = len;
        goto out;
    }
    mbedtls_x509_crt_init(crt);
    ret = mbedtls_x509_crt_parse_der(crt, der + der_cap - len, (size_t)len);
out:
    free(val);
    free(der);
    mbedtls_x509write_crt_free(&w);
    return ret;
}

int vbs_ratls_create(int (*f_rng)(void*, unsigned char*, size_t), void* p_rng,
                     enum vbs_ratls_test test, mbedtls_pk_context* key, mbedtls_x509_crt* crt,
                     struct vbs_ratls_info* info, const char** what) {
    const char* dummy;
    if (!what)
        what = &dummy;
    unsigned char* report = malloc(VBS_RATLS_REPORT_MAX);
    mbedtls_pk_context other;
    mbedtls_pk_init(&other);
    int ret;
    if (!report)
        return -ENOMEM;
    double t0 = now_ms();
    *what = "key generation";
    if ((ret = gen_key(key, f_rng, p_rng)))
        goto out;
    double t1 = now_ms();
    *what = "attestation report";
    size_t report_len = 0;
    if (test == VBS_RATLS_GENUINE) {
        ret = get_report(key, report, &report_len);
    } else {
        if ((ret = gen_key(&other, f_rng, p_rng)) || (ret = get_report(&other, report, &report_len)))
            goto out;
        if (test == VBS_RATLS_TEST_TAMPER)
            ret = vbs_ratls_spki_sha256(key, report + VBS_RATLS_USER_DATA_OFFSET);
    }
    if (ret)
        goto out;
    double t2 = now_ms();
    if ((ret = make_cert(key, report, report_len, f_rng, p_rng, crt, what)))
        goto out;
    if (info) {
        info->report_len = report_len;
        info->ms_key = t1 - t0;
        info->ms_report = t2 - t1;
        info->ms_cert = now_ms() - t2;
        ret = vbs_ratls_spki_sha256(key, info->key_sha256);
    }
out:
    mbedtls_pk_free(&other);
    free(report);
    return ret;
}
