/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/*
 * vbs-tls-client: a Windows TLS client (SChannel) that talks to a server in a VBS enclave only after
 * it verified the server's attestation (CI-Examples/vbs/tls-echo, Documentation/vbs.rst "TLS with
 * attestation"). `gramine-vbs tls-client ...` compiles it once with MSVC and runs it.
 *
 *   vbs-tls-client PORT --verifier DIR\IMAGE.dll [checks] [action]
 *
 * After the TLS handshake (TLS 1.2; SChannel checks that the server holds the certificate's private
 * key) and before any application data:
 *   1. the server's certificate (self-signed) must carry the VBS attestation report in the
 *      extension 2.25.51004656318469523056256505321196161405 (an OCTET STRING with the package);
 *   2. key binding: the report's user data must be SHA-256(the certificate's DER
 *      SubjectPublicKeyInfo) followed by 32 zero bytes;
 *   3. the report is verified by `pal_vbs_host.exe IMAGE.dll 1 verify` (next to IMAGE.dll): the
 *      platform signature (EnclaveVerifyAttestationReport inside a fresh enclave of IMAGE, so on
 *      this machine only), the user data of step 2, --same-image (unique ID and author ID of the
 *      report equal IMAGE's), the pinned family and image IDs, the minimum SVN, and no debug
 *      enclave unless --allow-debug.
 * Any failure: the connection is closed without sending anything, the exit code is 1.
 *
 * Checks:  --family-id HEX  --image-id HEX  --min-svn N  --allow-debug  --no-same-image
 * Actions (default: --message "hello from Windows"):
 *   --message TEXT        send TEXT, print the echo
 *   --bench N SIZE        N round trips of SIZE bytes; prints MB/s and latency
 *   --handshakes N        N more connections with full handshakes (timed; not re-verified)
 *   --plain               no TLS and no verification: the server's plain port (for comparisons)
 * Other: -h HOST (127.0.0.1), -v (print the verifier's output), --save-report FILE.
 */

#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#define SECURITY_WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <bcrypt.h>
#include <schannel.h>
#include <security.h>
#include <sspi.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "secur32.lib")
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "bcrypt.lib")

static const unsigned char REPORT_OID[] = {
    0x69, 0xcc, 0xdf, 0x91, 0xed, 0xb5, 0x97, 0xc2, 0x98, 0x85,
    0x8d, 0xa8, 0xf1, 0x8c, 0x99, 0xe5, 0x94, 0x92, 0x7d,
};
#define USER_DATA_OFFSET 32

static const char* g_host = "127.0.0.1";
static double g_qpf;

static double now_us(void) {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1e6 / g_qpf;
}

static SOCKET connect_to(int port) {
    struct addrinfo hints = {0}, *res = NULL;
    char ps[16];
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    snprintf(ps, sizeof(ps), "%d", port);
    if (getaddrinfo(g_host, ps, &hints, &res) != 0)
        return INVALID_SOCKET;
    SOCKET s = INVALID_SOCKET;
    for (struct addrinfo* a = res; a; a = a->ai_next) {
        s = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (s == INVALID_SOCKET)
            continue;
        if (connect(s, a->ai_addr, (int)a->ai_addrlen) == 0)
            break;
        closesocket(s);
        s = INVALID_SOCKET;
    }
    freeaddrinfo(res);
    if (s != INVALID_SOCKET) {
        BOOL one = TRUE;
        setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof(one));
    }
    return s;
}

static int send_all(SOCKET s, const char* p, size_t n) {
    while (n) {
        int k = send(s, p, (int)n, 0);
        if (k <= 0)
            return -1;
        p += k;
        n -= (size_t)k;
    }
    return 0;
}

/* ---- TLS with SChannel ---- */

struct tls {
    SOCKET s;
    CredHandle cred;
    CtxtHandle ctx;
    int have_ctx;
    SecPkgContext_StreamSizes sizes;
    char* in;          /* received, not yet decrypted */
    size_t in_len, in_cap;
    char* plain;       /* decrypted, not yet returned */
    size_t plain_pos, plain_len;
    char* out;         /* one encrypted record */
};

static int tls_fill(struct tls* t) {
    if (t->in_len == t->in_cap) {
        t->in_cap = t->in_cap ? t->in_cap * 2 : 65536;
        t->in = realloc(t->in, t->in_cap);
    }
    int k = recv(t->s, t->in + t->in_len, (int)(t->in_cap - t->in_len), 0);
    if (k <= 0)
        return -1;
    t->in_len += (size_t)k;
    return 0;
}

static int tls_handshake(struct tls* t) {
    SCHANNEL_CRED sc = {0};
    sc.dwVersion = SCHANNEL_CRED_VERSION;
    sc.grbitEnabledProtocols = SP_PROT_TLS1_2_CLIENT;
    /* the certificate is self-signed: it is checked by the attestation below, not by a CA */
    sc.dwFlags = SCH_CRED_MANUAL_CRED_VALIDATION | SCH_CRED_NO_DEFAULT_CREDS;
    TimeStamp ts;
    SECURITY_STATUS st = AcquireCredentialsHandleA(NULL, UNISP_NAME_A, SECPKG_CRED_OUTBOUND, NULL,
                                                   &sc, NULL, NULL, &t->cred, &ts);
    if (st != SEC_E_OK) {
        fprintf(stderr, "vbs-tls-client: AcquireCredentialsHandle 0x%08lx\n", st);
        return -1;
    }
    DWORD flags = ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT | ISC_REQ_CONFIDENTIALITY |
                  ISC_REQ_EXTENDED_ERROR | ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_STREAM |
                  ISC_REQ_MANUAL_CRED_VALIDATION;
    DWORD got_flags;
    int first = 1;
    for (;;) {
        SecBuffer inb[2] = { { (unsigned long)t->in_len, SECBUFFER_TOKEN, t->in },
                             { 0, SECBUFFER_EMPTY, NULL } };
        SecBufferDesc ind = { SECBUFFER_VERSION, 2, inb };
        SecBuffer outb[1] = { { 0, SECBUFFER_TOKEN, NULL } };
        SecBufferDesc outd = { SECBUFFER_VERSION, 1, outb };
        st = InitializeSecurityContextA(&t->cred, first ? NULL : &t->ctx,
                                        first ? "gramine-vbs-tls-echo" : NULL, flags, 0, 0,
                                        first ? NULL : &ind, 0, &t->ctx, &outd, &got_flags, &ts);
        if (first)
            t->have_ctx = 1;
        first = 0;
        if (outb[0].cbBuffer && outb[0].pvBuffer) {
            int r = send_all(t->s, outb[0].pvBuffer, outb[0].cbBuffer);
            FreeContextBuffer(outb[0].pvBuffer);
            if (r < 0)
                return -1;
        }
        if (st == SEC_E_INCOMPLETE_MESSAGE) {
            if (tls_fill(t) < 0)
                return -1;
            continue;
        }
        if (st == SEC_E_OK || st == SEC_I_CONTINUE_NEEDED) {
            /* keep what was not consumed */
            if (inb[1].BufferType == SECBUFFER_EXTRA && inb[1].cbBuffer) {
                memmove(t->in, t->in + t->in_len - inb[1].cbBuffer, inb[1].cbBuffer);
                t->in_len = inb[1].cbBuffer;
            } else {
                t->in_len = 0;
            }
            if (st == SEC_E_OK)
                break;
            if (t->in_len == 0 && tls_fill(t) < 0)
                return -1;
            continue;
        }
        fprintf(stderr, "vbs-tls-client: TLS handshake failed: 0x%08lx\n", st);
        return -1;
    }
    QueryContextAttributesA(&t->ctx, SECPKG_ATTR_STREAM_SIZES, &t->sizes);
    t->out = malloc(t->sizes.cbHeader + t->sizes.cbMaximumMessage + t->sizes.cbTrailer);
    return 0;
}

static int tls_send(struct tls* t, const char* p, size_t n) {
    while (n) {
        size_t k = n < t->sizes.cbMaximumMessage ? n : t->sizes.cbMaximumMessage;
        memcpy(t->out + t->sizes.cbHeader, p, k);
        SecBuffer b[4] = {
            { t->sizes.cbHeader, SECBUFFER_STREAM_HEADER, t->out },
            { (unsigned long)k, SECBUFFER_DATA, t->out + t->sizes.cbHeader },
            { t->sizes.cbTrailer, SECBUFFER_STREAM_TRAILER, t->out + t->sizes.cbHeader + k },
            { 0, SECBUFFER_EMPTY, NULL },
        };
        SecBufferDesc d = { SECBUFFER_VERSION, 4, b };
        if (EncryptMessage(&t->ctx, 0, &d, 0) != SEC_E_OK)
            return -1;
        if (send_all(t->s, t->out, b[0].cbBuffer + b[1].cbBuffer + b[2].cbBuffer) < 0)
            return -1;
        p += k;
        n -= k;
    }
    return 0;
}

/* up to cap decrypted bytes; 0 when the peer closed */
static int tls_recv(struct tls* t, char* buf, size_t cap) {
    while (t->plain_pos == t->plain_len) {
        if (t->in_len == 0 && tls_fill(t) < 0)
            return 0;
        SecBuffer b[4] = { { (unsigned long)t->in_len, SECBUFFER_DATA, t->in },
                           { 0, SECBUFFER_EMPTY, NULL }, { 0, SECBUFFER_EMPTY, NULL },
                           { 0, SECBUFFER_EMPTY, NULL } };
        SecBufferDesc d = { SECBUFFER_VERSION, 4, b };
        SECURITY_STATUS st = DecryptMessage(&t->ctx, &d, 0, NULL);
        if (st == SEC_E_INCOMPLETE_MESSAGE) {
            if (tls_fill(t) < 0)
                return 0;
            continue;
        }
        if (st == SEC_I_CONTEXT_EXPIRED)
            return 0; /* close_notify */
        if (st != SEC_E_OK) {
            fprintf(stderr, "vbs-tls-client: DecryptMessage 0x%08lx\n", st);
            return -1;
        }
        char* data = NULL;
        size_t dlen = 0, extra = 0;
        for (int i = 1; i < 4; i++) {
            if (b[i].BufferType == SECBUFFER_DATA) {
                data = b[i].pvBuffer;
                dlen = b[i].cbBuffer;
            } else if (b[i].BufferType == SECBUFFER_EXTRA) {
                extra = b[i].cbBuffer;
            }
        }
        /* the plain text lies inside t->in: copy it out before the extra bytes move */
        free(t->plain);
        t->plain = malloc(dlen + 1);
        memcpy(t->plain, data, dlen);
        t->plain_pos = 0;
        t->plain_len = dlen;
        if (extra)
            memmove(t->in, t->in + t->in_len - extra, extra);
        t->in_len = extra;
    }
    size_t k = t->plain_len - t->plain_pos < cap ? t->plain_len - t->plain_pos : cap;
    memcpy(buf, t->plain + t->plain_pos, k);
    t->plain_pos += k;
    return (int)k;
}

static void tls_close(struct tls* t) {
    if (t->have_ctx)
        DeleteSecurityContext(&t->ctx);
    FreeCredentialsHandle(&t->cred);
    closesocket(t->s);
    free(t->in);
    free(t->plain);
    free(t->out);
    memset(t, 0, sizeof(*t));
}

/* ---- the certificate: DER walking ---- */

/* reads one TLV at *p (< end): tag, the contents [*v, *v + *vlen); advances *p past it */
static int der_next(const uint8_t** p, const uint8_t* end, int* tag, const uint8_t** v,
                    size_t* vlen) {
    const uint8_t* q = *p;
    if (end - q < 2)
        return -1;
    *tag = q[0];
    size_t len = q[1];
    q += 2;
    if (len & 0x80) {
        int nb = (int)(len & 0x7f);
        if (nb < 1 || nb > 3 || end - q < nb)
            return -1;
        len = 0;
        for (int i = 0; i < nb; i++)
            len = len << 8 | *q++;
    }
    if ((size_t)(end - q) < len)
        return -1;
    *v = q;
    *vlen = len;
    *p = q + len;
    return 0;
}

/* the DER SubjectPublicKeyInfo (whole TLV) and the report extension's value of a certificate */
static int parse_cert(const uint8_t* der, size_t len, const uint8_t** spki, size_t* spki_len,
                      const uint8_t** report, size_t* report_len) {
    const uint8_t *p = der, *end = der + len, *v, *tv;
    size_t vl;
    int tag;
    *report = NULL;
    if (der_next(&p, end, &tag, &v, &vl) < 0 || tag != 0x30)
        return -1;
    const uint8_t* cp = v;
    const uint8_t* cend = v + vl;
    if (der_next(&cp, cend, &tag, &tv, &vl) < 0 || tag != 0x30) /* tbsCertificate */
        return -1;
    const uint8_t* t = tv;
    const uint8_t* tend = tv + vl;
    int field = 0;
    while (t < tend) {
        const uint8_t* start = t;
        if (der_next(&t, tend, &tag, &v, &vl) < 0)
            return -1;
        if (tag == 0xa0)
            continue; /* [0] version */
        field++;      /* 1 serial, 2 signature, 3 issuer, 4 validity, 5 subject, 6 spki */
        if (field == 6) {
            *spki = start;
            *spki_len = (size_t)(t - start);
        }
        if (tag == 0xa3) { /* [3] extensions: SEQUENCE OF Extension */
            const uint8_t* e = v;
            const uint8_t* eend = v + vl;
            if (der_next(&e, eend, &tag, &v, &vl) < 0 || tag != 0x30)
                return -1;
            e = v;
            eend = v + vl;
            while (e < eend) {
                const uint8_t *x, *oid, *val;
                size_t xl, oidl, vall;
                if (der_next(&e, eend, &tag, &x, &xl) < 0 || tag != 0x30)
                    return -1;
                const uint8_t* xe = x + xl;
                if (der_next(&x, xe, &tag, &oid, &oidl) < 0 || tag != 0x06)
                    return -1;
                if (der_next(&x, xe, &tag, &val, &vall) < 0)
                    return -1;
                if (tag == 0x01 && der_next(&x, xe, &tag, &val, &vall) < 0) /* critical */
                    return -1;
                if (tag != 0x04)
                    return -1;
                if (oidl == sizeof(REPORT_OID) && !memcmp(oid, REPORT_OID, oidl)) {
                    /* extnValue contains OCTET STRING { report } */
                    const uint8_t* r = val;
                    const uint8_t* rv;
                    size_t rl;
                    if (der_next(&r, val + vall, &tag, &rv, &rl) < 0 || tag != 0x04)
                        return -1;
                    *report = rv;
                    *report_len = rl;
                }
            }
        }
    }
    return field >= 6 ? 0 : -1;
}

static int sha256(const void* data, size_t len, uint8_t out[32]) {
    return BCryptHash(BCRYPT_SHA256_ALG_HANDLE, NULL, 0, (PUCHAR)data, (ULONG)len, out, 32) == 0
           ? 0 : -1;
}

static void hexstr(const uint8_t* b, size_t n, char* out) {
    for (size_t i = 0; i < n; i++)
        sprintf(out + 2 * i, "%02x", b[i]);
}

/* ---- verification ---- */

struct checks {
    const char* verifier;   /* path of the image (.dll) whose pal_vbs_host.exe verifies */
    const char* family_id;
    const char* image_id;
    long min_svn;
    int allow_debug, same_image, verbose;
    const char* save_report;
};

static int fail(const char* what) {
    printf("REFUSED: %s\n", what);
    return -1;
}

static int verify_peer(struct tls* t, const struct checks* c) {
    PCCERT_CONTEXT cert = NULL;
    if (QueryContextAttributesA(&t->ctx, SECPKG_ATTR_REMOTE_CERT_CONTEXT, &cert) != SEC_E_OK ||
            !cert)
        return fail("no server certificate");
    const uint8_t *spki = NULL, *report = NULL;
    size_t spki_len = 0, report_len = 0;
    int r = parse_cert(cert->pbCertEncoded, cert->cbCertEncoded, &spki, &spki_len, &report,
                       &report_len);
    uint8_t* rep = NULL;
    uint8_t hash[32];
    int ret = -1;
    if (r < 0 || !spki) {
        fail("the certificate does not parse");
        goto out;
    }
    if (!report) {
        fail("the certificate carries no attestation report");
        goto out;
    }
    rep = malloc(report_len);
    memcpy(rep, report, report_len);
    if (sha256(spki, spki_len, hash) < 0) {
        fail("SHA-256");
        goto out;
    }
    char hx[129];
    hexstr(hash, 32, hx);
    printf("server key: SHA-256 of the certificate's public key %s\n", hx);
    printf("report: %zu bytes in the certificate\n", report_len);
    if (c->save_report) {
        FILE* f = fopen(c->save_report, "wb");
        if (f) {
            fwrite(rep, 1, report_len, f);
            fclose(f);
        }
    }
    /* 2. key binding (also checked by the verifier, with the user data below) */
    uint8_t want[64] = {0};
    memcpy(want, hash, 32);
    if (report_len < USER_DATA_OFFSET + 64 || memcmp(rep + USER_DATA_OFFSET, want, 64)) {
        fail("key binding: the report's user data is not the hash of the TLS key");
        goto out;
    }
    printf("key binding: ok (user data = SHA-256(public key) || zeros)\n");

    /* 3. the report, verified by a fresh enclave of the verifying image */
    char tmp[MAX_PATH], file[MAX_PATH];
    GetTempPathA(MAX_PATH, tmp);
    GetTempFileNameA(tmp, "vbsr", 0, file);
    FILE* f = fopen(file, "wb");
    if (!f) {
        fail("cannot write the report to a temporary file");
        goto out;
    }
    fwrite(rep, 1, report_len, f);
    fclose(f);
    char dir[MAX_PATH], exe[MAX_PATH + 32];
    snprintf(dir, sizeof(dir), "%s", c->verifier);
    char* slash = strrchr(dir, '\\');
    if (slash)
        slash[1] = 0;
    else
        strcpy(dir, ".\\");
    snprintf(exe, sizeof(exe), "%spal_vbs_host.exe", dir);
    char want_hex[129];
    hexstr(want, 64, want_hex);
    char cmd[4096];
    int n = snprintf(cmd, sizeof(cmd), "\"\"%s\" \"%s\" 1 verify \"%s\" --user-data %s%s", exe,
                     c->verifier, file, want_hex, c->same_image ? " --same-image" : "");
    if (c->family_id)
        n += snprintf(cmd + n, sizeof(cmd) - n, " --family-id %s", c->family_id);
    if (c->image_id)
        n += snprintf(cmd + n, sizeof(cmd) - n, " --image-id %s", c->image_id);
    if (c->min_svn >= 0)
        n += snprintf(cmd + n, sizeof(cmd) - n, " --min-svn %ld", c->min_svn);
    if (c->allow_debug)
        n += snprintf(cmd + n, sizeof(cmd) - n, " --allow-debug");
    snprintf(cmd + n, sizeof(cmd) - n, " 2>&1\"");
    double t0 = now_us();
    FILE* p = _popen(cmd, "r");
    if (!p) {
        DeleteFileA(file);
        fail("cannot run the verifier");
        goto out;
    }
    char line[1024];
    int failed_lines = 0;
    while (fgets(line, sizeof(line), p)) {
        if (!strncmp(line, "[host]", 6))
            continue;
        if (strstr(line, "FAILED")) {
            failed_lines++;
            printf("  %s", line);
        } else if (c->verbose) {
            printf("  %s", line);
        }
    }
    int rc = _pclose(p);
    DeleteFileA(file);
    printf("verifier: exit %d in %.0f ms (pal_vbs_host.exe verify)\n", rc, (now_us() - t0) / 1e3);
    if (rc != 0 || failed_lines) {
        fail("the attestation report did not verify (see the checks marked FAILED)");
        goto out;
    }
    printf("attestation: VERIFIED (platform signature on this machine, key binding, identity)\n");
    ret = 0;
out:
    free(rep);
    CertFreeCertificateContext(cert);
    return ret;
}

/* ---- actions ---- */

static int cmp_double(const void* a, const void* b) {
    double x = *(const double*)a, y = *(const double*)b;
    return x < y ? -1 : x > y;
}

typedef int (*send_fn)(void* c, const char* p, size_t n);
typedef int (*recv_fn)(void* c, char* p, size_t n);
static int plain_send(void* c, const char* p, size_t n) { return send_all(*(SOCKET*)c, p, n); }
static int plain_recv(void* c, char* p, size_t n) { return recv(*(SOCKET*)c, p, (int)n, 0); }
static int t_send(void* c, const char* p, size_t n) { return tls_send(c, p, n); }
static int t_recv(void* c, char* p, size_t n) { return tls_recv(c, p, n); }

static int bench(void* c, send_fn sf, recv_fn rf, long rounds, size_t size, const char* label) {
    char* buf = malloc(size);
    char* back = malloc(size);
    memset(buf, 'e', size);
    double* lat = malloc(sizeof(double) * (size_t)rounds);
    double t0 = now_us();
    for (long i = 0; i < rounds; i++) {
        double s = now_us();
        if (sf(c, buf, size) < 0)
            return fail("send failed");
        size_t got = 0;
        while (got < size) {
            int k = rf(c, back + got, size - got);
            if (k <= 0)
                return fail("the server closed the connection");
            got += (size_t)k;
        }
        lat[i] = now_us() - s;
    }
    double secs = (now_us() - t0) / 1e6;
    qsort(lat, (size_t)rounds, sizeof(double), cmp_double);
    printf("%s echo: %ld round trips of %zu bytes in %.2f s: %.1f MB/s each way, latency ms p50 "
           "%.3f p99 %.3f\n", label, rounds, size, secs, rounds * (double)size / secs / 1e6,
           lat[rounds / 2] / 1e3, lat[(long)(rounds * 0.99)] / 1e3);
    free(buf);
    free(back);
    free(lat);
    return 0;
}

int main(int argc, char** argv) {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_qpf = (double)f.QuadPart;
    setvbuf(stdout, NULL, _IONBF, 0);
    struct checks c = { .min_svn = -1, .same_image = 1 };
    const char* message = "hello from Windows";
    long bench_n = 0, handshakes = 0;
    size_t bench_size = 0;
    int plain = 0;
    if (argc < 2) {
        fprintf(stderr, "usage: vbs-tls-client PORT --verifier IMAGE.dll [options] (see the source)\n");
        return 2;
    }
    int port = atoi(argv[1]);
    for (int i = 2; i < argc; i++) {
        const char* o = argv[i];
        const char* v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(o, "--verifier") && v) { c.verifier = v; i++; }
        else if (!strcmp(o, "--family-id") && v) { c.family_id = v; i++; }
        else if (!strcmp(o, "--image-id") && v) { c.image_id = v; i++; }
        else if (!strcmp(o, "--min-svn") && v) { c.min_svn = atol(v); i++; }
        else if (!strcmp(o, "--allow-debug")) c.allow_debug = 1;
        else if (!strcmp(o, "--no-same-image")) c.same_image = 0;
        else if (!strcmp(o, "--save-report") && v) { c.save_report = v; i++; }
        else if (!strcmp(o, "--message") && v) { message = v; i++; }
        else if (!strcmp(o, "--bench") && v && i + 2 < argc) {
            bench_n = atol(v);
            bench_size = (size_t)atol(argv[i + 2]);
            i += 2;
        }
        else if (!strcmp(o, "--handshakes") && v) { handshakes = atol(v); i++; }
        else if (!strcmp(o, "--plain")) plain = 1;
        else if (!strcmp(o, "-h") && v) { g_host = v; i++; }
        else if (!strcmp(o, "-v")) c.verbose = 1;
        else { fprintf(stderr, "vbs-tls-client: unknown option %s\n", o); return 2; }
    }
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    if (plain) {
        SOCKET s = connect_to(port);
        if (s == INVALID_SOCKET)
            return fail("cannot connect"), 1;
        int rc = bench_n ? bench(&s, plain_send, plain_recv, bench_n, bench_size, "plain") : 0;
        closesocket(s);
        return rc ? 1 : 0;
    }
    if (!c.verifier) {
        fprintf(stderr, "vbs-tls-client: --verifier IMAGE.dll is required\n");
        return 2;
    }
    struct tls t = {0};
    double t0 = now_us();
    t.s = connect_to(port);
    if (t.s == INVALID_SOCKET)
        return fail("cannot connect"), 1;
    if (tls_handshake(&t) < 0) {
        tls_close(&t);
        return fail("TLS handshake"), 1;
    }
    SecPkgContext_ConnectionInfo ci;
    QueryContextAttributesA(&t.ctx, SECPKG_ATTR_CONNECTION_INFO, &ci);
    printf("TLS: handshake in %.2f ms (protocol 0x%lx, cipher 0x%x, %lu bits)\n",
           (now_us() - t0) / 1e3, ci.dwProtocol, ci.aiCipher, ci.dwCipherStrength);
    if (verify_peer(&t, &c) < 0) {
        tls_close(&t);
        return 1;
    }
    int rc = 0;
    if (bench_n) {
        rc = bench(&t, t_send, t_recv, bench_n, bench_size, "TLS");
    } else {
        size_t n = strlen(message);
        if (tls_send(&t, message, n) < 0) {
            rc = fail("send failed");
        } else {
            char* back = malloc(n + 1);
            size_t got = 0;
            while (got < n) {
                int k = tls_recv(&t, back + got, n - got);
                if (k <= 0)
                    break;
                got += (size_t)k;
            }
            back[got] = 0;
            printf("echo: %s\n", back);
            rc = got == n && !memcmp(back, message, n) ? 0 : fail("wrong echo");
            free(back);
        }
    }
    tls_close(&t);
    if (handshakes > 0) {
        double* lat = malloc(sizeof(double) * (size_t)handshakes);
        for (long i = 0; i < handshakes; i++) {
            struct tls u = {0};
            double s = now_us();
            u.s = connect_to(port);
            if (u.s == INVALID_SOCKET || tls_handshake(&u) < 0) {
                tls_close(&u);
                return fail("handshake failed"), 1;
            }
            lat[i] = now_us() - s;
            tls_close(&u);
        }
        qsort(lat, (size_t)handshakes, sizeof(double), cmp_double);
        double sum = 0;
        for (long i = 0; i < handshakes; i++)
            sum += lat[i];
        printf("handshakes: %ld (connect + TLS 1.2 handshake, no verification): avg %.2f ms p50 "
               "%.2f ms p99 %.2f ms\n", handshakes, sum / handshakes / 1e3,
               lat[handshakes / 2] / 1e3, lat[(long)(handshakes * 0.99)] / 1e3);
        free(lat);
    }
    return rc ? 1 : 0;
}
