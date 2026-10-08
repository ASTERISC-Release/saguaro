/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2017 Fortanix, Inc.
 * Copyright (C) 2021 Intel Corp.
 */

/* This mbedTLS config is for v3.6.0 and assumes Intel x86-64 CPU with AESNI and SSE2 support */

#pragma once

/* mbedTLS v3.6.0 by default enables the following TLS 1.3 features:
 *
 * #define MBEDTLS_SSL_PROTO_TLS1_3
 * #define MBEDTLS_SSL_TLS1_3_COMPATIBILITY_MODE
 * #define MBEDTLS_SSL_TLS1_3_KEY_EXCHANGE_MODE_PSK_ENABLED
 *
 * These features are currently *not* enabled in mbedTLS version used for internal Gramine PAL
 * crypto/TLS.
 * TODO: analyze their impact and add the applicable ones
 */

#if defined(__x86_64__)
#define MBEDTLS_AESNI_C
#define MBEDTLS_AES_USE_HARDWARE_ONLY
#endif
/* No aarch64 equivalent of the two x86_64-only defines above for now (ARMv8 Crypto Extension AES
 * would be MBEDTLS_AESCE_C, a different mechanism, not just a renamed flag) -- mbedtls falls
 * back to its portable software AES implementation there, same reasoning as the AES-NI hardware
 * check guarded off in pal_main.c's verify_hw_requirements(). */
#define MBEDTLS_AES_C
#define MBEDTLS_ASN1_PARSE_C
#define MBEDTLS_ASN1_WRITE_C
#define MBEDTLS_BASE64_C
#define MBEDTLS_BIGNUM_C
#define MBEDTLS_CIPHER_C
#define MBEDTLS_CMAC_C
#define MBEDTLS_CTR_DRBG_C
#define MBEDTLS_DHM_C
#define MBEDTLS_ENTROPY_C
#define MBEDTLS_ENTROPY_HARDWARE_ALT
#define MBEDTLS_ERROR_C
#define MBEDTLS_GCM_C
#define MBEDTLS_GENPRIME
#define MBEDTLS_HAVE_ASM
#if defined(__x86_64__)
#define MBEDTLS_HAVE_SSE2
#define MBEDTLS_HAVE_X86_64
#endif
#define MBEDTLS_HKDF_C
#define MBEDTLS_KEY_EXCHANGE_PSK_ENABLED
#define MBEDTLS_MD_C
#define MBEDTLS_NET_C
#define MBEDTLS_NO_PLATFORM_ENTROPY
#define MBEDTLS_NO_UDBL_DIVISION
#define MBEDTLS_OID_C
#define MBEDTLS_PKCS1_V15
#define MBEDTLS_PLATFORM_C
#define MBEDTLS_PLATFORM_ZEROIZE_ALT
#define MBEDTLS_RSA_C
#define MBEDTLS_SHA256_C
#define MBEDTLS_SSL_CIPHERSUITES MBEDTLS_TLS_PSK_WITH_AES_128_GCM_SHA256
#define MBEDTLS_SSL_CLI_C
#define MBEDTLS_SSL_CONTEXT_SERIALIZATION
#define MBEDTLS_SSL_PROTO_TLS1_2
#define MBEDTLS_SSL_SRV_C
#define MBEDTLS_SSL_TLS_C
