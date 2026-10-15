/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/*
 * Host requests of the vbs PAL proper (files and sockets of the untrusted host, served through
 * W's host_call service and PAL_VBS_OCALL_HOST) and the manifest's host-file policy:
 *
 *   vbs.trusted_files = [ { path = "/data/in.txt", sha256 = "<64 hex digits>" }, ... ]
 *       read-only host files; the whole file is read and checked against its hash when it is
 *       opened, and served from enclave memory afterwards (the host cannot change what is read);
 *   vbs.allowed_files = [ "/data/out/", ... ]
 *       path prefixes passed through to the host without any protection (as sgx.allowed_files).
 *
 * Any other path that is not a packed file does not exist for the application.
 */

#include "api.h"
#include "crypto.h"
#include "pal.h"
#include "pal_error.h"
#include "pal_internal.h"
#include "pal_vbs.h"
#include "toml.h"
#include "toml_utils.h"

struct vbs_trusted_file* g_vbs_trusted_files;
size_t g_vbs_trusted_files_cnt;
char** g_vbs_allowed_files;
size_t g_vbs_allowed_files_cnt;

int64_t vbs_hostcall_v(uint64_t op, int64_t a0, int64_t a1, int64_t a2, int64_t a3,
                       const void* in_pre, size_t in_pre_len, const struct iovec* in_iov,
                       size_t in_niov, void* out_pre, size_t out_pre_cap,
                       const struct iovec* out_iov, size_t out_niov, size_t* out_len) {
    _Static_assert(sizeof(struct iovec) == sizeof(struct pal_vbs_iov), "iovec layout");
    if (in_pre_len > PAL_VBS_HOST_DATA_MAX)
        return -22; /* -EINVAL */
    struct pal_vbs_hostreq req;
    memset(&req, 0, sizeof(req));
    req.op = op;
    req.a[0] = a0;
    req.a[1] = a1;
    req.a[2] = a2;
    req.a[3] = a3;
    struct pal_vbs_hostcall_v c = {
        .req = &req,
        .in_pre = in_pre,
        .in_pre_len = in_pre_len,
        .in_iov = (const struct pal_vbs_iov*)in_iov,
        .in_niov = in_niov,
        .out_pre = out_pre,
        .out_pre_cap = out_pre_cap,
        .out_iov = (const struct pal_vbs_iov*)out_iov,
        .out_niov = out_niov,
    };
    int64_t ret = (int64_t)pal_vbs_wcall(g_vbs->host_call_v, (uint64_t)&c, 0, 0);
    if (out_len)
        *out_len = c.out_len;
    return ret;
}

int64_t vbs_hostcall(uint64_t op, int64_t a0, int64_t a1, int64_t a2, int64_t a3,
                     const void* in, size_t in_len, void* out, size_t out_cap, size_t* out_len) {
    if (in_len > PAL_VBS_HOST_DATA_MAX)
        return -22; /* -EINVAL */
    return vbs_hostcall_v(op, a0, a1, a2, a3, in, in_len, NULL, 0, out, out_cap, NULL, 0,
                          out_len);
}

static int hexval(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

int vbs_init_host_files(void) {
    toml_table_t* vbs = toml_table_in(g_pal_public_state.manifest_root, "vbs");
    if (!vbs)
        return 0;

    toml_array_t* tf = toml_array_in(vbs, "trusted_files");
    if (tf) {
        int n = toml_array_nelem(tf);
        g_vbs_trusted_files = calloc(n > 0 ? n : 1, sizeof(*g_vbs_trusted_files));
        if (!g_vbs_trusted_files)
            return -PAL_ERROR_NOMEM;
        for (int i = 0; i < n; i++) {
            toml_table_t* t = toml_table_at(tf, i);
            char* path = NULL;
            char* hash = NULL;
            if (!t || toml_string_in(t, "path", &path) < 0 || !path ||
                    toml_string_in(t, "sha256", &hash) < 0 || !hash || strlen(hash) != 64) {
                log_error("vbs.trusted_files[%d]: needs path and a 64-digit sha256", i);
                return -PAL_ERROR_INVAL;
            }
            for (int k = 0; k < 32; k++) {
                int hi = hexval(hash[2 * k]), lo = hexval(hash[2 * k + 1]);
                if (hi < 0 || lo < 0) {
                    log_error("vbs.trusted_files[%d]: sha256 is not hexadecimal", i);
                    return -PAL_ERROR_INVAL;
                }
                g_vbs_trusted_files[i].sha256[k] = (uint8_t)(hi << 4 | lo);
            }
            free(hash);
            g_vbs_trusted_files[i].path = path;
        }
        g_vbs_trusted_files_cnt = n;
    }

    toml_array_t* af = toml_array_in(vbs, "allowed_files");
    if (af) {
        int n = toml_array_nelem(af);
        g_vbs_allowed_files = calloc(n > 0 ? n : 1, sizeof(*g_vbs_allowed_files));
        if (!g_vbs_allowed_files)
            return -PAL_ERROR_NOMEM;
        for (int i = 0; i < n; i++) {
            toml_datum_t d = toml_string_at(af, i);
            if (!d.ok || d.u.s[0] != '/') {
                log_error("vbs.allowed_files[%d]: must be an absolute path", i);
                return -PAL_ERROR_INVAL;
            }
            g_vbs_allowed_files[i] = d.u.s;
        }
        g_vbs_allowed_files_cnt = n;
    }
    if (g_vbs_allowed_files_cnt)
        log_warning("vbs: vbs.allowed_files are passed to the host without any protection");
    return 0;
}

const struct vbs_trusted_file* vbs_find_trusted(const char* path) {
    for (size_t i = 0; i < g_vbs_trusted_files_cnt; i++)
        if (!strcmp(g_vbs_trusted_files[i].path, path))
            return &g_vbs_trusted_files[i];
    return NULL;
}

/* `path` is an allowed prefix ending in '/' (a directory and all below it), or equal to one */
bool vbs_is_allowed(const char* path) {
    size_t len = strlen(path);
    for (size_t i = 0; i < g_vbs_allowed_files_cnt; i++) {
        const char* p = g_vbs_allowed_files[i];
        size_t pl = strlen(p);
        if (pl && p[pl - 1] == '/') {
            if (!strncmp(path, p, pl) || (len == pl - 1 && !strncmp(path, p, len)))
                return true;
        } else if (!strcmp(path, p)) {
            return true;
        }
    }
    return false;
}
