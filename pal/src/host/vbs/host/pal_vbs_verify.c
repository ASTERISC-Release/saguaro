/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 Gramine contributors */

/*
 * The host's `verify` mode: checks a VBS attestation report package (what /dev/attestation/report
 * and /dev/attestation/quote return inside the enclave).
 *
 *   pal_vbs_host.exe <enclave.dll> 1 verify <report file> [checks]
 *
 * 1. The package's structure: header (version 1, RSA-PSS SHA-256), sizes, the report and its
 *    module entries.
 * 2. The platform signature: a fresh enclave of <enclave.dll> (any signed VBS enclave would do)
 *    calls EnclaveVerifyAttestationReport (E_VerifyReport). This checks the signature chain to the
 *    Secure Kernel of THIS machine; it says nothing about another machine (that needs Microsoft
 *    Azure Attestation and the machine's boot log; Documentation/attestation.rst).
 * 3. The checks asked for:
 *      --user-data HEX      the report's 64 bytes of user data (HEX shorter than 64 bytes is
 *                           padded with zero bytes, as /dev/attestation/user_report_data does)
 *      --user-data-file F   the same, from the bytes of a file
 *      --same-image         the report's UniqueId (image measurement) and AuthorId (signer) are those
 *                           of <enclave.dll> as loaded for the verification
 *      --unique-id HEX, --author-id HEX   pin the identity explicitly (32 bytes each)
 *      --min-svn N          the enclave's SVN is at least N
 *      --allow-debug        accept a debuggable enclave (refused otherwise)
 * Exit code 0 when every check passes, 1 otherwise, 2 for usage errors.
 */

#include <windows.h>
#include <enclaveapi.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pal_vbs_abi.h"

/* the package layout (ntenclv.h of the Windows SDK, an enclave-side header; repeated here) */
#pragma pack(push, 1)
struct pkg_header {
    uint32_t package_size, version, signature_scheme, signed_statement_size, signature_size,
        reserved;
};
struct identity {
    uint8_t owner_id[32], unique_id[32], author_id[32], family_id[16], image_id[16];
    uint32_t enclave_svn, secure_kernel_svn, platform_svn, flags, signing_level, enclave_type;
};
struct report {
    uint32_t report_size, report_version;
    uint8_t enclave_data[64];
    struct identity id;
};
struct vardata_header {
    uint32_t data_type, size;
};
struct module {
    struct vardata_header h;
    uint8_t unique_id[32], author_id[32], family_id[16], image_id[16];
    uint32_t svn;
    /* WCHAR name[] follows */
};
#pragma pack(pop)

#define FLAG_FULL_DEBUG_ENABLED    0x1
#define FLAG_DYNAMIC_DEBUG_ENABLED 0x2

static void hex(const char* label, const uint8_t* b, size_t n) {
    printf("  %-18s ", label);
    for (size_t i = 0; i < n; i++)
        printf("%02x", b[i]);
    printf("\n");
}

static int parse_hex(const char* s, uint8_t* out, size_t cap, size_t* out_len) {
    size_t n = strlen(s);
    if (n % 2 || n / 2 > cap)
        return -1;
    for (size_t i = 0; i < n / 2; i++) {
        unsigned v;
        if (sscanf(s + 2 * i, "%2x", &v) != 1)
            return -1;
        out[i] = (uint8_t)v;
    }
    *out_len = n / 2;
    return 0;
}

static int g_failed;

static void check(int ok, const char* what) {
    printf("[verify] %-58s %s\n", what, ok ? "ok" : "FAILED");
    if (!ok)
        g_failed = 1;
}

int pal_vbs_verify_main(void* enclave, int argc, char** argv) {
    if (argc < 5) {
        fprintf(stderr, "usage: pal_vbs_host.exe <enclave.dll> 1 verify <report file> [checks]\n");
        return 2;
    }
    FILE* f = fopen(argv[4], "rb");
    if (!f) {
        fprintf(stderr, "[verify] cannot open %s\n", argv[4]);
        return 2;
    }
    static uint8_t pkg[PAL_VBS_REPORT_MAX];
    size_t size = fread(pkg, 1, sizeof(pkg), f);
    fclose(f);

    /* the checks asked for */
    uint8_t want_data[64] = {0}, want_unique[32], want_author[32];
    int have_data = 0, same_image = 0, have_unique = 0, have_author = 0, allow_debug = 0;
    long min_svn = -1;
    for (int i = 5; i < argc; i++) {
        size_t n = 0;
        if (!strcmp(argv[i], "--user-data") && i + 1 < argc) {
            if (parse_hex(argv[++i], want_data, sizeof(want_data), &n) < 0)
                return 2;
            have_data = 1;
        } else if (!strcmp(argv[i], "--user-data-file") && i + 1 < argc) {
            FILE* u = fopen(argv[++i], "rb");
            if (!u)
                return 2;
            n = fread(want_data, 1, sizeof(want_data), u);
            fclose(u);
            have_data = 1;
        } else if (!strcmp(argv[i], "--unique-id") && i + 1 < argc) {
            if (parse_hex(argv[++i], want_unique, 32, &n) < 0 || n != 32)
                return 2;
            have_unique = 1;
        } else if (!strcmp(argv[i], "--author-id") && i + 1 < argc) {
            if (parse_hex(argv[++i], want_author, 32, &n) < 0 || n != 32)
                return 2;
            have_author = 1;
        } else if (!strcmp(argv[i], "--min-svn") && i + 1 < argc) {
            min_svn = atol(argv[++i]);
        } else if (!strcmp(argv[i], "--same-image")) {
            same_image = 1;
        } else if (!strcmp(argv[i], "--allow-debug")) {
            allow_debug = 1;
        } else {
            fprintf(stderr, "[verify] unknown option %s\n", argv[i]);
            return 2;
        }
    }

    /* 1. structure */
    const struct pkg_header* h = (const struct pkg_header*)pkg;
    int structure_ok = size >= sizeof(*h) + sizeof(struct report) && h->package_size == size &&
                       h->version == 1 && h->signature_scheme == 1 &&
                       (uint64_t)sizeof(*h) + h->signed_statement_size + h->signature_size == size &&
                       h->signed_statement_size >= sizeof(struct report);
    check(structure_ok, "package structure (version 1, RSA-PSS SHA-256, sizes)");
    if (!structure_ok)
        return 1;
    const struct report* r = (const struct report*)(pkg + sizeof(*h));
    structure_ok = r->report_size <= h->signed_statement_size && r->report_version == 1;
    check(structure_ok, "report header");
    if (!structure_ok)
        return 1;

    printf("[verify] package %zu bytes: signed statement %u, signature %u\n", size,
           h->signed_statement_size, h->signature_size);
    hex("user data", r->enclave_data, sizeof(r->enclave_data));
    hex("unique id", r->id.unique_id, 32);
    hex("author id", r->id.author_id, 32);
    hex("family id", r->id.family_id, 16);
    hex("image id", r->id.image_id, 16);
    hex("owner id", r->id.owner_id, 32);
    printf("  %-18s enclave %u, secure kernel %u, platform %u\n", "svn", r->id.enclave_svn,
           r->id.secure_kernel_svn, r->id.platform_svn);
    printf("  %-18s 0x%x%s, signing level %u, enclave type 0x%x\n", "flags", r->id.flags,
           (r->id.flags & FLAG_FULL_DEBUG_ENABLED) ? " (debuggable)" : "", r->id.signing_level,
           r->id.enclave_type);
    const uint8_t* v = (const uint8_t*)r + sizeof(*r);
    const uint8_t* end = (const uint8_t*)r + r->report_size;
    while (v + sizeof(struct vardata_header) <= end) {
        const struct vardata_header* vh = (const struct vardata_header*)v;
        if (vh->size < sizeof(*vh) || v + vh->size > end)
            break;
        if (vh->data_type == 1 && vh->size >= sizeof(struct module)) {
            const struct module* m = (const struct module*)v;
            const WCHAR* name = (const WCHAR*)(v + sizeof(*m));
            size_t nl = (vh->size - sizeof(*m)) / sizeof(WCHAR);
            printf("  module             %.*ls (svn %u)\n", (int)wcsnlen(name, nl), name, m->svn);
        }
        v += vh->size;
    }

    /* 2. platform signature, in a fresh enclave */
    PENCLAVE_ROUTINE fn = (PENCLAVE_ROUTINE)GetProcAddress((HMODULE)enclave, "E_VerifyReport");
    if (!fn) {
        check(0, "E_VerifyReport present in the verifying image");
        return 1;
    }
    struct pal_vbs_verify req;
    memset(&req, 0, sizeof(req));
    req.report = (uint64_t)(uintptr_t)pkg;
    req.report_size = size;
    void* out = NULL;
    BOOL ok = CallEnclave(fn, &req, TRUE, &out);
    int sig_ok = ok && out == 0 && req.hresult == 0;
    printf("[verify] EnclaveVerifyAttestationReport: 0x%08llx\n",
           (unsigned long long)(uint32_t)req.hresult);
    check(sig_ok, "platform signature (EnclaveVerifyAttestationReport)");

    /* 3. the checks asked for */
    if (have_data)
        check(!memcmp(want_data, r->enclave_data, 64), "user data matches");
    if (same_image) {
        const struct identity* own = (const struct identity*)req.own_identity;
        check(req.info_hresult == 0 && !memcmp(own->unique_id, r->id.unique_id, 32),
              "unique id = the verifying image's");
        check(req.info_hresult == 0 && !memcmp(own->author_id, r->id.author_id, 32),
              "author id = the verifying image's");
    }
    if (have_unique)
        check(!memcmp(want_unique, r->id.unique_id, 32), "unique id matches");
    if (have_author)
        check(!memcmp(want_author, r->id.author_id, 32), "author id matches");
    if (min_svn >= 0)
        check(r->id.enclave_svn >= (uint32_t)min_svn, "enclave svn >= the minimum");
    check(allow_debug || !(r->id.flags & (FLAG_FULL_DEBUG_ENABLED | FLAG_DYNAMIC_DEBUG_ENABLED)),
          allow_debug ? "debug enclave (allowed)" : "not a debuggable enclave");

    printf("[verify] result: %s\n", g_failed ? "REJECTED" : "VERIFIED (locally, this machine)");
    return g_failed ? 1 : 0;
}
