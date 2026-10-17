/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/*
 * SEV-SNP attestation reports through the guest-request protocol (SEV-SNP firmware ABI, chapter
 * "Guest messages"; GHCB spec, SNP Guest Request): the request is encrypted and authenticated with
 * AES-256-GCM under VMPCK0, a key that only the guest and the SNP firmware know (it is in the
 * secrets page that the firmware wrote at launch), and passes the hypervisor in shared pages.
 * The firmware answers with a report that it signs with the chip's VCEK and that carries the launch
 * measurement, the policy, the TCB and the 64 bytes of report data that the guest chose.
 */

#include "api.h"
#include "pal_error.h"
#include "pal_internal.h"
#include "spinlock.h"

#include "mbedtls/gcm.h"

#include "kernel_memory.h"
#include "sev.h"
#include "sev_ghcb.h"

#define SNP_SECRETS_VMPCK0_OFFSET    0x20
#define SNP_SECRETS_MSG_SEQNO0_OFFSET 0xa0

#define SNP_MSG_REPORT_REQ 5
#define SNP_MSG_REPORT_RSP 6
#define SNP_AEAD_AES_256_GCM 1

struct snp_guest_msg_hdr {
    uint8_t  authtag[32];
    uint64_t msg_seqno;
    uint8_t  rsvd1[8];
    uint8_t  algo;
    uint8_t  hdr_version;
    uint16_t hdr_sz;
    uint8_t  msg_type;
    uint8_t  msg_version;
    uint16_t msg_sz;
    uint32_t rsvd2;
    uint8_t  msg_vmpck;
    uint8_t  rsvd3[35];
} __attribute__((packed));

_Static_assert(sizeof(struct snp_guest_msg_hdr) == 0x60, "SNP message header");

struct snp_report_req {
    uint8_t  user_data[64];
    uint32_t vmpl;
    uint8_t  rsvd[28];
} __attribute__((packed));

struct snp_report_resp {
    uint32_t status;
    uint32_t report_size;
    uint8_t  rsvd[24];
    uint8_t  report[SEV_SNP_REPORT_SIZE];
} __attribute__((packed));

#define SNP_MSG_PAYLOAD_MAX (4096 - sizeof(struct snp_guest_msg_hdr))

static spinlock_t g_snp_req_lock = INIT_SPINLOCK_UNLOCKED;
static uint64_t g_snp_seqno; /* sequence number of the last message (0: none yet) */
static uint8_t* g_snp_req_page;  /* shared */
static uint8_t* g_snp_resp_page; /* shared */

static int snp_aes_gcm(bool encrypt, const uint8_t key[32], uint64_t seqno, const uint8_t* aad,
                       size_t aad_size, const uint8_t* in, uint8_t* out, size_t size,
                       uint8_t tag[16]) {
    uint8_t iv[12] = {0};
    memcpy(iv, &seqno, sizeof(seqno));

    mbedtls_gcm_context gcm;
    mbedtls_gcm_init(&gcm);
    int ret = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, 256);
    if (ret == 0) {
        if (encrypt)
            ret = mbedtls_gcm_crypt_and_tag(&gcm, MBEDTLS_GCM_ENCRYPT, size, iv, sizeof(iv), aad,
                                            aad_size, in, out, 16, tag);
        else
            ret = mbedtls_gcm_auth_decrypt(&gcm, size, iv, sizeof(iv), aad, aad_size, tag, 16, in,
                                           out);
    }
    mbedtls_gcm_free(&gcm);
    return ret == 0 ? 0 : -PAL_ERROR_DENIED;
}

int sev_snp_get_report(const uint8_t user_data[64], uint8_t report[SEV_SNP_REPORT_SIZE]) {
    if (!sev_snp_active())
        return -PAL_ERROR_NOTIMPLEMENTED;

    const uint8_t* secrets = (const uint8_t*)SEV_SNP_SECRETS_PAGE_GPA;
    const uint8_t* vmpck0 = secrets + SNP_SECRETS_VMPCK0_OFFSET;

    int ret;
    spinlock_lock(&g_snp_req_lock);

    if (!g_snp_req_page) {
        g_snp_req_page = memory_get_shared_region(PAGE_SIZE);
        g_snp_resp_page = memory_get_shared_region(PAGE_SIZE);
        if (!g_snp_req_page || !g_snp_resp_page) {
            ret = -PAL_ERROR_NOMEM;
            goto out;
        }
        /* the firmware counts from the value it left in the secrets page (0 at launch) */
        g_snp_seqno = *(const uint32_t*)(secrets + SNP_SECRETS_MSG_SEQNO0_OFFSET);
    }

    uint64_t seqno = g_snp_seqno + 1;

    struct snp_report_req req = {0};
    memcpy(req.user_data, user_data, sizeof(req.user_data));
    req.vmpl = 0;

    __attribute__((aligned(64))) uint8_t msg[PAGE_SIZE];
    memset(msg, 0, sizeof(msg));
    struct snp_guest_msg_hdr* hdr = (struct snp_guest_msg_hdr*)msg;
    hdr->msg_seqno = seqno;
    hdr->algo = SNP_AEAD_AES_256_GCM;
    hdr->hdr_version = 1;
    hdr->hdr_sz = sizeof(*hdr);
    hdr->msg_type = SNP_MSG_REPORT_REQ;
    hdr->msg_version = 1;
    hdr->msg_sz = sizeof(req);
    hdr->msg_vmpck = 0;
    /* the authenticated data is the header from `algo` on */
    ret = snp_aes_gcm(/*encrypt=*/true, vmpck0, seqno, &hdr->algo,
                      sizeof(*hdr) - offsetof(struct snp_guest_msg_hdr, algo), (uint8_t*)&req,
                      msg + sizeof(*hdr), sizeof(req), hdr->authtag);
    if (ret < 0)
        goto out;

    memcpy(g_snp_req_page, msg, PAGE_SIZE);
    memset(g_snp_resp_page, 0, PAGE_SIZE);

    uint64_t flags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(flags) : : "memory");
    struct ghcb* ghcb = sev_this_ghcb();
    memset(ghcb->valid_bitmap, 0, sizeof(ghcb->valid_bitmap));
    ret = sev_ghcb_call(ghcb, SVM_VMGEXIT_GUEST_REQUEST, (uint64_t)g_snp_req_page,
                        (uint64_t)g_snp_resp_page);
    uint64_t exit_info_2 = ghcb->sw_exit_info_2;
    if (flags & 0x200)
        __asm__ volatile("sti" : : : "memory");
    if (ret < 0 || exit_info_2) {
        log_error("SEV-SNP: guest request failed (exit info 2: 0x%lx)", exit_info_2);
        ret = -PAL_ERROR_DENIED;
        goto out;
    }

    /* the response: copy out of shared memory first, then check it */
    memcpy(msg, g_snp_resp_page, PAGE_SIZE);
    if (hdr->msg_seqno != seqno + 1 || hdr->algo != SNP_AEAD_AES_256_GCM ||
            hdr->hdr_sz != sizeof(*hdr) || hdr->msg_type != SNP_MSG_REPORT_RSP ||
            hdr->msg_sz != sizeof(struct snp_report_resp) || hdr->msg_vmpck != 0) {
        log_error("SEV-SNP: malformed guest-request response (seqno %lu, type %u, size %u)",
                  hdr->msg_seqno, hdr->msg_type, hdr->msg_sz);
        ret = -PAL_ERROR_DENIED;
        goto out;
    }
    struct snp_report_resp resp;
    ret = snp_aes_gcm(/*encrypt=*/false, vmpck0, seqno + 1, &hdr->algo,
                      sizeof(*hdr) - offsetof(struct snp_guest_msg_hdr, algo),
                      msg + sizeof(*hdr), (uint8_t*)&resp, sizeof(resp), hdr->authtag);
    if (ret < 0) {
        log_error("SEV-SNP: the guest-request response does not authenticate");
        goto out;
    }
    /* both sequence numbers are used up, also if the report itself is an error */
    g_snp_seqno = seqno + 1;
    if (resp.status != 0 || resp.report_size != SEV_SNP_REPORT_SIZE) {
        log_error("SEV-SNP: the firmware refused the report (status 0x%x, size %u)", resp.status,
                  resp.report_size);
        ret = -PAL_ERROR_DENIED;
        goto out;
    }
    memcpy(report, resp.report, SEV_SNP_REPORT_SIZE);
    ret = 0;
out:
    spinlock_unlock(&g_snp_req_lock);
    return ret;
}
