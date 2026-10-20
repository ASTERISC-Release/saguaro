/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/*
 * tls-echo: a TLS echo server for a Windows VBS enclave whose certificate carries the enclave's
 * attestation report (Documentation/vbs.md, "TLS with attestation"; README.md of this folder).
 *
 * At start, inside the enclave, the optional helper ../common/vbs-ratls.c generates an ECDSA P-256
 * key pair, gets the VBS attestation report bound to it (user data = SHA-256 of the public key's
 * DER SubjectPublicKeyInfo, then 32 zero bytes) and makes a self-signed certificate that carries
 * the report (an X.509 extension). The private key never leaves the enclave; a client that verifies
 * the report and the binding knows that the TLS peer is that enclave.
 *
 * Then it serves, one connection at a time: TLS on --port (default 4433), and the same echo without
 * TLS on --plain-port (default 4434, for cost comparisons). Each connection echoes every byte back
 * until the client closes.
 *
 * Test options (they make the server present what an attacker on the host could present, for the
 * client's negative tests; they need loader.insecure__use_cmdline_argv):
 *   --test-wrong-key      the report is bound to a second key, the TLS key is another one
 *   --test-tamper-report  as --test-wrong-key, then the report's user data is overwritten with the
 *                         TLS key's hash (the platform signature no longer matches)
 */

#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/error.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/pk.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"

#include "vbs-ratls.h"

static mbedtls_entropy_context g_entropy;
static mbedtls_ctr_drbg_context g_drbg;

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static void die_mbed(const char* what, int ret) {
    char buf[200];
    mbedtls_strerror(ret, buf, sizeof(buf));
    fprintf(stderr, "tls-echo: %s: -0x%04x %s\n", what, (unsigned)-ret, buf);
    exit(1);
}

static int listen_on(int port) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((unsigned short)port),
                             .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    if (bind(s, (struct sockaddr*)&a, sizeof(a)) || listen(s, 16)) {
        fprintf(stderr, "tls-echo: cannot listen on 127.0.0.1:%d: %s\n", port, strerror(errno));
        exit(1);
    }
    return s;
}

static void serve_plain(int c) {
    static char buf[1 << 16];
    for (;;) {
        ssize_t n = recv(c, buf, sizeof(buf), 0);
        if (n <= 0)
            break;
        for (ssize_t off = 0; off < n;) {
            ssize_t w = send(c, buf + off, (size_t)(n - off), 0);
            if (w <= 0)
                return;
            off += w;
        }
    }
}

static void serve_tls(int c, mbedtls_ssl_config* conf, int verbose) {
    mbedtls_ssl_context ssl;
    mbedtls_net_context net = { .fd = c };
    mbedtls_ssl_init(&ssl);
    if (mbedtls_ssl_setup(&ssl, conf))
        goto out;
    mbedtls_ssl_set_bio(&ssl, &net, mbedtls_net_send, mbedtls_net_recv, NULL);
    double t0 = now_ms();
    int ret;
    while ((ret = mbedtls_ssl_handshake(&ssl)) != 0) {
        if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
            if (verbose) {
                char e[160];
                mbedtls_strerror(ret, e, sizeof(e));
                printf("tls-echo: handshake failed: -0x%04x %s\n", (unsigned)-ret, e);
            }
            goto out;
        }
    }
    if (verbose)
        printf("tls-echo: handshake %.2f ms, %s %s\n", now_ms() - t0, mbedtls_ssl_get_version(&ssl),
               mbedtls_ssl_get_ciphersuite(&ssl));
    static unsigned char buf[1 << 14];
    for (;;) {
        ret = mbedtls_ssl_read(&ssl, buf, sizeof(buf));
        if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE ||
                ret == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET)
            continue;
        if (ret <= 0)
            break;
        int n = ret;
        for (int off = 0; off < n;) {
            ret = mbedtls_ssl_write(&ssl, buf + off, (size_t)(n - off));
            if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE)
                continue;
            if (ret <= 0)
                goto out;
            off += ret;
        }
    }
    mbedtls_ssl_close_notify(&ssl);
out:
    mbedtls_ssl_free(&ssl);
}

int main(int argc, char** argv) {
    int port = 4433, plain_port = 4434, wrong_key = 0, tamper = 0, verbose = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--port") && i + 1 < argc)
            port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--plain-port") && i + 1 < argc)
            plain_port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--test-wrong-key"))
            wrong_key = 1;
        else if (!strcmp(argv[i], "--test-tamper-report"))
            wrong_key = tamper = 1;
        else if (!strcmp(argv[i], "--verbose"))
            verbose = 1;
        else {
            fprintf(stderr, "tls-echo: unknown option %s\n", argv[i]);
            return 2;
        }
    }
    setvbuf(stdout, NULL, _IOLBF, 0);

    double t0 = now_ms();
    mbedtls_entropy_init(&g_entropy);
    mbedtls_ctr_drbg_init(&g_drbg);
    int ret = mbedtls_ctr_drbg_seed(&g_drbg, mbedtls_entropy_func, &g_entropy,
                                    (const unsigned char*)"gramine-vbs-tls-echo", 20);
    if (ret)
        die_mbed("ctr_drbg_seed", ret);
    double t_seed = now_ms();

    mbedtls_pk_context key;
    mbedtls_x509_crt crt;
    struct vbs_ratls_info info;
    const char* what = NULL;
    enum vbs_ratls_test test = tamper ? VBS_RATLS_TEST_TAMPER
                               : wrong_key ? VBS_RATLS_TEST_WRONG_KEY : VBS_RATLS_GENUINE;
    if ((ret = vbs_ratls_create(mbedtls_ctr_drbg_random, &g_drbg, test, &key, &crt, &info, &what)))
        die_mbed(what, ret);
    if (test != VBS_RATLS_GENUINE)
        printf("tls-echo: TEST MODE: the report is %s\n",
               tamper ? "bound to another key and its user data overwritten"
                      : "bound to another key than the TLS key");
    printf("tls-echo: key pair generated in the enclave (ECDSA P-256) in %.1f ms (DRBG seeding "
           "%.1f ms); report %zu bytes in %.1f ms; certificate in %.1f ms\n", info.ms_key,
           t_seed - t0, info.report_len, info.ms_report, info.ms_cert);
    printf("tls-echo: SHA-256 of the public key: ");
    for (int i = 0; i < 32; i++)
        printf("%02x", info.key_sha256[i]);
    printf("\n");

    mbedtls_ssl_config conf;
    mbedtls_ssl_config_init(&conf);
    if ((ret = mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_SERVER, MBEDTLS_SSL_TRANSPORT_STREAM,
                                           MBEDTLS_SSL_PRESET_DEFAULT)))
        die_mbed("ssl_config_defaults", ret);
    mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &g_drbg);
    if ((ret = mbedtls_ssl_conf_own_cert(&conf, &crt, &key)))
        die_mbed("ssl_conf_own_cert", ret);

    int ls = listen_on(port), lp = listen_on(plain_port);
    printf("tls-echo: listening on 127.0.0.1:%d (TLS) and 127.0.0.1:%d (plain)\n", port, plain_port);
    for (;;) {
        struct pollfd pf[2] = { { .fd = ls, .events = POLLIN }, { .fd = lp, .events = POLLIN } };
        if (poll(pf, 2, -1) <= 0)
            continue;
        for (int k = 0; k < 2; k++) {
            if (!(pf[k].revents & POLLIN))
                continue;
            int c = accept(pf[k].fd, NULL, NULL);
            if (c < 0)
                continue;
            int one = 1;
            setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
            if (k == 0)
                serve_tls(c, &conf, verbose);
            else
                serve_plain(c);
            close(c);
        }
    }
    return 0;
}
