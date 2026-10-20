/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/*
 * vbs-ratls: an OPTIONAL helper for applications in a Windows VBS enclave that add TLS themselves
 * and want a client to verify that the TLS peer is the attested enclave (dev docs: vbs-enclave/manual.md,
 * "TLS with attestation (optional, per application)"). Nothing in Gramine uses it by itself: an
 * application compiles vbs-ratls.c and links mbedtls only if it chooses to (CI-Examples/vbs/tls-echo
 * is the example).
 *
 * vbs_ratls_create() generates an ECDSA P-256 key pair inside the enclave (randomness from the
 * caller's mbedtls DRBG, seeded from getrandom(), which the LibOS serves from the enclave's own
 * generator), obtains the VBS attestation report whose user data is SHA-256(DER
 * SubjectPublicKeyInfo of the public key) followed by 32 zero bytes (/dev/attestation), and makes a
 * self-signed X.509 certificate for the key that carries the report in a non-critical extension
 * (VBS_RATLS_OID: an OCTET STRING holding the report package). The key never leaves the enclave.
 */

#pragma once

#include <stddef.h>

#include "mbedtls/pk.h"
#include "mbedtls/x509_crt.h"

/* 2.25.51004656318469523056256505321196161405 (a UUID OID; Windows' CryptoAPI shows it as an empty
 * name because its arc exceeds 32 bits, so clients locate it in the DER themselves) */
#define VBS_RATLS_OID_DER                                                                     \
    "\x69\xcc\xdf\x91\xed\xb5\x97\xc2\x98\x85\x8d\xa8\xf1\x8c\x99\xe5\x94\x92\x7d"
#define VBS_RATLS_OID_LEN 19

#define VBS_RATLS_REPORT_MAX       16384
#define VBS_RATLS_USER_DATA_OFFSET 32 /* in the package: header (24 bytes), report size, version */

/* test modes: what an attacker on the host could present instead (for clients' negative tests) */
enum vbs_ratls_test {
    VBS_RATLS_GENUINE = 0,
    VBS_RATLS_TEST_WRONG_KEY,    /* the report is bound to another key than the certificate's */
    VBS_RATLS_TEST_TAMPER,       /* as WRONG_KEY, then the report's user data overwritten with the
                                  * certificate key's hash (its signature no longer matches) */
};

struct vbs_ratls_info {
    size_t report_len;
    unsigned char key_sha256[32];
    double ms_key, ms_report, ms_cert; /* time spent in each step */
};

/* key and crt are initialised here; returns 0 or a negative mbedtls error or -errno; `what` (if not
 * NULL) names the step that failed */
int vbs_ratls_create(int (*f_rng)(void*, unsigned char*, size_t), void* p_rng,
                     enum vbs_ratls_test test, mbedtls_pk_context* key, mbedtls_x509_crt* crt,
                     struct vbs_ratls_info* info, const char** what);

/* SHA-256 of the DER SubjectPublicKeyInfo of `pk` */
int vbs_ratls_spki_sha256(mbedtls_pk_context* pk, unsigned char out[32]);
