/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 Gramine contributors */

/*
 * vbs-net: a small Winsock client for the checks and measurements of applications that run in a
 * VBS enclave (Documentation/vbs.rst, "Makefile flow"). The enclave's sockets are Windows sockets
 * of the host process, and WSL2 in its default (NAT) networking mode cannot reach Windows'
 * loopback, so clients run on Windows. `gramine-vbs net ...` compiles this file once with MSVC
 * and runs it (from WSL through interop).
 *
 *   vbs-net wait  PORT [SECONDS]               connect until it works (exit 0) or time out (1)
 *   vbs-net req   PORT DATA [MS]               send DATA (C escapes \r \n \t \\ \xHH), close the
 *                                              sending side, print what comes back until the peer
 *                                              closes or MS milliseconds (default 5000) pass
 *   vbs-net redis PORT CMD [ARG...]            one Redis command (RESP), reply printed like
 *                                              redis-cli: text, (integer) N, (nil), (error) E,
 *                                              arrays as "1) ..." lines
 *   vbs-net bench KIND PORT [options]          load generator; KIND: redis-ping, redis-set,
 *                                              redis-get, memcached-set, memcached-get, http
 *       -c N    connections (threads), default 8     -n N   requests in all, default 100000
 *       -P N    requests pipelined per round trip    -d N   value size in bytes, default 64
 *       -k N    key space (keys key:0..N-1), default 1000    -p PATH  HTTP path, default /
 *       -w N    warm-up requests per connection (not counted), default 100
 *     prints one line: requests, errors, seconds, requests/s, latency (ms) avg p50 p99 max of
 *     each round trip (one round trip carries P requests).
 *   Options for all: -h HOST (default 127.0.0.1).
 */

#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma comment(lib, "ws2_32.lib")

static const char* g_host = "127.0.0.1";

static SOCKET connect_to(int port, int quiet) {
    struct addrinfo hints = {0}, *res = NULL;
    char ps[16];
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    snprintf(ps, sizeof(ps), "%d", port);
    if (getaddrinfo(g_host, ps, &hints, &res) != 0) {
        if (!quiet)
            fprintf(stderr, "vbs-net: cannot resolve %s\n", g_host);
        return INVALID_SOCKET;
    }
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
    if (s == INVALID_SOCKET && !quiet)
        fprintf(stderr, "vbs-net: cannot connect to %s:%d (WSA %d)\n", g_host, port,
                WSAGetLastError());
    if (s != INVALID_SOCKET) {
        BOOL one = TRUE;
        setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof(one));
    }
    return s;
}

static int send_all(SOCKET s, const char* p, size_t n) {
    while (n) {
        int k = send(s, p, (int)(n > 1 << 20 ? 1 << 20 : n), 0);
        if (k <= 0)
            return -1;
        p += k;
        n -= (size_t)k;
    }
    return 0;
}

/* ---- a buffered reader ---- */

struct rd {
    SOCKET s;
    char buf[65536];
    size_t pos, len;
};

static int rd_fill(struct rd* r) {
    if (r->pos == r->len)
        r->pos = r->len = 0;
    if (r->len == sizeof(r->buf)) {
        if (r->pos == 0)
            return -1;
        memmove(r->buf, r->buf + r->pos, r->len - r->pos);
        r->len -= r->pos;
        r->pos = 0;
    }
    int k = recv(r->s, r->buf + r->len, (int)(sizeof(r->buf) - r->len), 0);
    if (k <= 0)
        return -1;
    r->len += (size_t)k;
    return 0;
}

/* a line ending in \r\n into out (without it); returns its length or -1 */
static int rd_line(struct rd* r, char* out, size_t outsz) {
    for (;;) {
        for (size_t i = r->pos; i + 1 < r->len; i++) {
            if (r->buf[i] == '\r' && r->buf[i + 1] == '\n') {
                size_t n = i - r->pos;
                if (out) {
                    size_t c = n < outsz - 1 ? n : outsz - 1;
                    memcpy(out, r->buf + r->pos, c);
                    out[c] = 0;
                }
                r->pos = i + 2;
                return (int)n;
            }
        }
        if (rd_fill(r) < 0)
            return -1;
    }
}

/* skip (or copy to stdout) n bytes */
static int rd_skip(struct rd* r, size_t n, int print) {
    while (n) {
        if (r->pos == r->len && rd_fill(r) < 0)
            return -1;
        size_t c = r->len - r->pos < n ? r->len - r->pos : n;
        if (print)
            fwrite(r->buf + r->pos, 1, c, stdout);
        r->pos += c;
        n -= c;
    }
    return 0;
}

/* ---- Redis replies ---- */

static int redis_reply(struct rd* r, int print, int depth, int* is_err) {
    char line[1024];
    int n = rd_line(r, line, sizeof(line));
    if (n < 1)
        return -1;
    switch (line[0]) {
        case '+':
            if (print)
                printf("%s\n", line + 1);
            return 0;
        case '-':
            if (is_err)
                *is_err = 1;
            if (print)
                printf("(error) %s\n", line + 1);
            return 0;
        case ':':
            if (print)
                printf("(integer) %s\n", line + 1);
            return 0;
        case '$': {
            long len = atol(line + 1);
            if (len < 0) {
                if (print)
                    printf("(nil)\n");
                return 0;
            }
            if (rd_skip(r, (size_t)len, print) < 0 || rd_skip(r, 2, 0) < 0)
                return -1;
            if (print)
                printf("\n");
            return 0;
        }
        case '*': {
            long cnt = atol(line + 1);
            if (cnt < 0) {
                if (print)
                    printf("(nil)\n");
                return 0;
            }
            if (cnt == 0 && print)
                printf("(empty array)\n");
            for (long i = 0; i < cnt; i++) {
                if (print)
                    printf("%*s%ld) ", depth * 3, "", i + 1);
                if (redis_reply(r, print, depth + 1, is_err) < 0)
                    return -1;
            }
            return 0;
        }
    }
    return -1;
}

static void redis_append_cmd(char** buf, size_t* len, size_t* cap, int argc, const char** argv,
                             const size_t* lens) {
    size_t need = 32;
    for (int i = 0; i < argc; i++)
        need += lens[i] + 32;
    if (*len + need > *cap) {
        *cap = (*len + need) * 2;
        *buf = realloc(*buf, *cap);
    }
    *len += (size_t)sprintf(*buf + *len, "*%d\r\n", argc);
    for (int i = 0; i < argc; i++) {
        *len += (size_t)sprintf(*buf + *len, "$%zu\r\n", lens[i]);
        memcpy(*buf + *len, argv[i], lens[i]);
        *len += lens[i];
        memcpy(*buf + *len, "\r\n", 2);
        *len += 2;
    }
}

/* ---- memcached and HTTP replies ---- */

static int memcached_reply(struct rd* r, int is_get, int* is_err) {
    char line[512];
    for (;;) {
        if (rd_line(r, line, sizeof(line)) < 0)
            return -1;
        if (!is_get) {
            if (strcmp(line, "STORED"))
                *is_err = 1;
            return 0;
        }
        if (!strcmp(line, "END"))
            return 0;
        if (!strncmp(line, "VALUE ", 6)) {
            /* VALUE <key> <flags> <bytes> */
            char* last = strrchr(line, ' ');
            long n = last ? atol(last + 1) : -1;
            if (n < 0 || rd_skip(r, (size_t)n + 2, 0) < 0)
                return -1;
            continue;
        }
        *is_err = 1;
        return 0;
    }
}

static int http_reply(struct rd* r, int* is_err) {
    char line[1024];
    long clen = -1;
    if (rd_line(r, line, sizeof(line)) < 0)
        return -1;
    if (strncmp(line, "HTTP/1.", 7) || strncmp(line + 9, "200", 3))
        *is_err = 1;
    for (;;) {
        int n = rd_line(r, line, sizeof(line));
        if (n < 0)
            return -1;
        if (n == 0)
            break;
        if (!_strnicmp(line, "Content-Length:", 15))
            clen = atol(line + 15);
    }
    if (clen < 0)
        return -1; /* the load generator needs a length (no chunked replies) */
    return rd_skip(r, (size_t)clen, 0);
}

/* ---- the load generator ---- */

enum kind { K_PING, K_SET, K_GET, K_MSET, K_MGET, K_HTTP };

struct bench {
    enum kind kind;
    int port, conns, pipeline, keys, warmup;
    long total;
    size_t dsize;
    const char* path;
    volatile LONG64 errors;
};

struct worker {
    struct bench* b;
    HANDLE thread;
    long count;      /* round trips */
    double* lat;     /* microseconds per round trip */
    long nlat;
    int failed;
    unsigned seed;
};

static double g_qpf;

static double now_us(void) {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1e6 / g_qpf;
}

static size_t build_round(struct worker* w, char** buf, size_t* cap, char* value) {
    struct bench* b = w->b;
    size_t len = 0;
    for (int i = 0; i < b->pipeline; i++) {
        char key[32];
        w->seed = w->seed * 1103515245u + 12345u;
        int klen = sprintf(key, "key:%u", (w->seed >> 8) % (unsigned)b->keys);
        if (*cap < len + b->dsize + 512) {
            *cap = (len + b->dsize + 512) * 2;
            *buf = realloc(*buf, *cap);
        }
        switch (b->kind) {
            case K_PING: {
                const char* a[] = {"PING"};
                size_t l[] = {4};
                redis_append_cmd(buf, &len, cap, 1, a, l);
                break;
            }
            case K_SET: {
                const char* a[] = {"SET", key, value};
                size_t l[] = {3, (size_t)klen, b->dsize};
                redis_append_cmd(buf, &len, cap, 3, a, l);
                break;
            }
            case K_GET: {
                const char* a[] = {"GET", key};
                size_t l[] = {3, (size_t)klen};
                redis_append_cmd(buf, &len, cap, 2, a, l);
                break;
            }
            case K_MSET:
                len += (size_t)sprintf(*buf + len, "set %s 0 0 %zu\r\n", key, b->dsize);
                memcpy(*buf + len, value, b->dsize);
                len += b->dsize;
                memcpy(*buf + len, "\r\n", 2);
                len += 2;
                break;
            case K_MGET:
                len += (size_t)sprintf(*buf + len, "get %s\r\n", key);
                break;
            case K_HTTP:
                len += (size_t)sprintf(*buf + len,
                                       "GET %s HTTP/1.1\r\nHost: %s\r\nConnection: keep-alive\r\n\r\n",
                                       b->path, g_host);
                break;
        }
    }
    return len;
}

static int read_round(struct worker* w, struct rd* r) {
    struct bench* b = w->b;
    for (int i = 0; i < b->pipeline; i++) {
        int err = 0, rc;
        switch (b->kind) {
            case K_MSET: rc = memcached_reply(r, 0, &err); break;
            case K_MGET: rc = memcached_reply(r, 1, &err); break;
            case K_HTTP: rc = http_reply(r, &err); break;
            default: rc = redis_reply(r, 0, 0, &err); break;
        }
        if (rc < 0)
            return -1;
        if (err)
            InterlockedIncrement64(&b->errors);
    }
    return 0;
}

static DWORD WINAPI worker_main(void* arg) {
    struct worker* w = arg;
    struct bench* b = w->b;
    struct rd* r = calloc(1, sizeof(*r));
    char* value = malloc(b->dsize + 1);
    memset(value, 'x', b->dsize);
    size_t cap = 4096;
    char* buf = malloc(cap);
    r->s = connect_to(b->port, 0);
    if (r->s == INVALID_SOCKET) {
        w->failed = 1;
        return 0;
    }
    for (int i = 0; i < b->warmup; i++) {
        size_t len = build_round(w, &buf, &cap, value);
        if (send_all(r->s, buf, len) < 0 || read_round(w, r) < 0) {
            w->failed = 1;
            return 0;
        }
    }
    w->lat = malloc(sizeof(double) * (size_t)(w->count + 1));
    for (long i = 0; i < w->count; i++) {
        size_t len = build_round(w, &buf, &cap, value);
        double t0 = now_us();
        if (send_all(r->s, buf, len) < 0 || read_round(w, r) < 0) {
            fprintf(stderr, "vbs-net: connection lost after %ld round trips\n", i);
            w->failed = 1;
            break;
        }
        w->lat[w->nlat++] = now_us() - t0;
    }
    closesocket(r->s);
    return 0;
}

static int cmp_double(const void* a, const void* b) {
    double x = *(const double*)a, y = *(const double*)b;
    return x < y ? -1 : x > y;
}

static int bench_main(int argc, char** argv) {
    struct bench b = {0};
    if (argc < 4)
        return 2;
    const char* k = argv[2];
    if (!strcmp(k, "redis-ping")) b.kind = K_PING;
    else if (!strcmp(k, "redis-set")) b.kind = K_SET;
    else if (!strcmp(k, "redis-get")) b.kind = K_GET;
    else if (!strcmp(k, "memcached-set")) b.kind = K_MSET;
    else if (!strcmp(k, "memcached-get")) b.kind = K_MGET;
    else if (!strcmp(k, "http")) b.kind = K_HTTP;
    else { fprintf(stderr, "vbs-net: unknown kind %s\n", k); return 2; }
    b.port = atoi(argv[3]);
    b.conns = 8; b.total = 100000; b.pipeline = 1; b.dsize = 64; b.keys = 1000; b.warmup = 100;
    b.path = "/";
    for (int i = 4; i + 1 < argc; i += 2) {
        const char* o = argv[i];
        const char* v = argv[i + 1];
        if (!strcmp(o, "-c")) b.conns = atoi(v);
        else if (!strcmp(o, "-n")) b.total = atol(v);
        else if (!strcmp(o, "-P")) b.pipeline = atoi(v);
        else if (!strcmp(o, "-d")) b.dsize = (size_t)atol(v);
        else if (!strcmp(o, "-k")) b.keys = atoi(v);
        else if (!strcmp(o, "-p")) b.path = v;
        else if (!strcmp(o, "-w")) b.warmup = atoi(v);
        else { fprintf(stderr, "vbs-net: unknown option %s\n", o); return 2; }
    }
    if (b.conns < 1 || b.pipeline < 1 || b.keys < 1)
        return 2;
    long rounds = b.total / b.pipeline;
    struct worker* w = calloc((size_t)b.conns, sizeof(*w));
    for (int i = 0; i < b.conns; i++) {
        w[i].b = &b;
        w[i].count = rounds / b.conns + (i < rounds % b.conns);
        w[i].seed = 12345u + (unsigned)i * 7919u;
    }
    double t0 = now_us();
    for (int i = 0; i < b.conns; i++)
        w[i].thread = CreateThread(NULL, 0, worker_main, &w[i], 0, NULL);
    long nlat = 0;
    int failed = 0;
    for (int i = 0; i < b.conns; i++) {
        WaitForSingleObject(w[i].thread, INFINITE);
        nlat += w[i].nlat;
        failed |= w[i].failed;
    }
    double secs = (now_us() - t0) / 1e6;
    double* all = malloc(sizeof(double) * (size_t)(nlat + 1));
    long j = 0;
    double sum = 0;
    for (int i = 0; i < b.conns; i++)
        for (long x = 0; x < w[i].nlat; x++) {
            all[j++] = w[i].lat[x];
            sum += w[i].lat[x];
        }
    qsort(all, (size_t)nlat, sizeof(double), cmp_double);
    long reqs = nlat * b.pipeline;
    printf("%s: %ld requests (%d connections, pipeline %d, %zu-byte values), %lld errors, "
           "%.2f s, %.0f requests/s, latency ms avg %.3f p50 %.3f p99 %.3f max %.3f\n",
           k, reqs, b.conns, b.pipeline, b.dsize, (long long)b.errors, secs,
           secs > 0 ? reqs / secs : 0.0, nlat ? sum / nlat / 1000 : 0.0,
           nlat ? all[nlat / 2] / 1000 : 0.0, nlat ? all[(long)(nlat * 0.99)] / 1000 : 0.0,
           nlat ? all[nlat - 1] / 1000 : 0.0);
    return failed || b.errors ? 1 : 0;
}

/* ---- the other commands ---- */

static size_t unescape(const char* in, char* out) {
    size_t n = 0;
    while (*in) {
        if (*in != '\\' || !in[1]) {
            out[n++] = *in++;
            continue;
        }
        in++;
        switch (*in) {
            case 'r': out[n++] = '\r'; in++; break;
            case 'n': out[n++] = '\n'; in++; break;
            case 't': out[n++] = '\t'; in++; break;
            case '\\': out[n++] = '\\'; in++; break;
            case 'x': {
                char h[3] = {in[1], in[1] ? in[2] : 0, 0};
                out[n++] = (char)strtol(h, NULL, 16);
                in += 1 + strlen(h);
                break;
            }
            default: out[n++] = '\\'; break;
        }
    }
    return n;
}

int main(int argc, char** argv) {
    WSADATA wsa;
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_qpf = (double)f.QuadPart;
    /* -h HOST anywhere */
    for (int i = 1; i + 1 < argc; i++) {
        if (!strcmp(argv[i], "-h")) {
            g_host = argv[i + 1];
            memmove(&argv[i], &argv[i + 2], sizeof(char*) * (size_t)(argc - i - 1));
            argc -= 2;
            break;
        }
    }
    if (argc < 3) {
        fprintf(stderr, "usage: vbs-net wait|req|redis|bench ... (see the source)\n");
        return 2;
    }
    if (WSAStartup(MAKEWORD(2, 2), &wsa))
        return 3;
    setvbuf(stdout, NULL, _IOFBF, 1 << 16);
    const char* cmd = argv[1];
    if (!strcmp(cmd, "wait")) {
        int port = atoi(argv[2]);
        double limit = argc > 3 ? atof(argv[3]) : 60;
        double t0 = now_us();
        for (;;) {
            SOCKET s = connect_to(port, 1);
            if (s != INVALID_SOCKET) {
                closesocket(s);
                printf("port %d open after %.2f s\n", port, (now_us() - t0) / 1e6);
                return 0;
            }
            if ((now_us() - t0) / 1e6 > limit) {
                fprintf(stderr, "vbs-net: port %d not open after %.0f s\n", port, limit);
                return 1;
            }
            Sleep(100);
        }
    }
    if (!strcmp(cmd, "req")) {
        if (argc < 4)
            return 2;
        SOCKET s = connect_to(atoi(argv[2]), 0);
        if (s == INVALID_SOCKET)
            return 1;
        char* data = malloc(strlen(argv[3]) + 1);
        size_t n = unescape(argv[3], data);
        DWORD ms = argc > 4 ? (DWORD)atol(argv[4]) : 5000;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&ms, sizeof(ms));
        if (send_all(s, data, n) < 0)
            return 1;
        shutdown(s, SD_SEND);
        char buf[65536];
        int k;
        while ((k = recv(s, buf, sizeof(buf), 0)) > 0)
            fwrite(buf, 1, (size_t)k, stdout);
        closesocket(s);
        return 0;
    }
    if (!strcmp(cmd, "redis")) {
        if (argc < 4)
            return 2;
        SOCKET s = connect_to(atoi(argv[2]), 0);
        if (s == INVALID_SOCKET)
            return 1;
        int n = argc - 3;
        size_t* lens = malloc(sizeof(size_t) * (size_t)n);
        for (int i = 0; i < n; i++)
            lens[i] = strlen(argv[3 + i]);
        char* buf = NULL;
        size_t len = 0, cap = 0;
        redis_append_cmd(&buf, &len, &cap, n, (const char**)&argv[3], lens);
        if (send_all(s, buf, len) < 0)
            return 1;
        struct rd* r = calloc(1, sizeof(*r));
        r->s = s;
        int err = 0;
        if (redis_reply(r, 1, 0, &err) < 0) {
            fprintf(stderr, "vbs-net: no complete reply\n");
            return 1;
        }
        closesocket(s);
        return err ? 1 : 0;
    }
    if (!strcmp(cmd, "bench"))
        return bench_main(argc, argv);
    fprintf(stderr, "vbs-net: unknown command %s\n", cmd);
    return 2;
}
