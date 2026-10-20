/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/*
 * A minimal NETLINK_ROUTE socket: it answers the two dump requests glibc's if_nameindex() and
 * getifaddrs() make (RTM_GETLINK, RTM_GETADDR) with one interface, the loopback `lo` (index 1,
 * 127.0.0.1/8 and ::1), and refuses everything else with NLMSG_ERROR. Nothing reaches the host:
 * the socket has no PAL handle, each request's reply is queued in the handle's peek buffer and
 * delivered by the next recvmsg() (one datagram per request, with the request's sequence number
 * and the socket's port id, as the kernel does).
 *
 * Why: Wine's nsiproxy.sys enumerates the interfaces for GetAdaptersAddresses() with
 * if_nameindex() and dereferences its result without a NULL check; without netlink it crashed the
 * driver thread and the calling program waited for ever (dev docs: proton-vm/FINDINGS.md,
 * 2026-10-16). Programs that need the real interfaces of the host are not served by this.
 */

#include <linux/netlink.h>
#include <linux/rtnetlink.h>

#include "libos_handle.h"
#include "libos_internal.h"
#include "libos_process.h"
#include "libos_socket.h"
#include "linux_socket.h"

/* from <linux/if.h> and <linux/if_arp.h>, which clash with other headers of the LibOS */
#define NL_IFF_UP       0x1
#define NL_IFF_LOOPBACK 0x8
#define NL_IFF_RUNNING  0x40
#define NL_IFF_LOWER_UP 0x10000
#define NL_ARPHRD_LOOPBACK 772
#ifndef SOL_NETLINK
#define SOL_NETLINK 270
#endif

#define LO_INDEX 1
#define LO_MTU   65536

#define REPLY_MAX 1024

static uint32_t netlink_port(struct libos_sock_handle* sock) {
    struct sockaddr_nl* nl = (struct sockaddr_nl*)&sock->local_addr;
    return nl->nl_pid ? nl->nl_pid : (uint32_t)g_process.pid;
}

static void set_local_addr(struct libos_sock_handle* sock, uint32_t pid, uint32_t groups) {
    struct sockaddr_nl* nl = (struct sockaddr_nl*)&sock->local_addr;
    memset(nl, 0, sizeof(*nl));
    nl->nl_family = AF_NETLINK;
    nl->nl_pid = pid;
    nl->nl_groups = groups;
    sock->local_addrlen = sizeof(*nl);
}

static int create(struct libos_handle* handle) {
    struct libos_sock_handle* sock = &handle->info.sock;
    /* the port id a kernel socket would get at its first send (autobind): this process's pid */
    set_local_addr(sock, (uint32_t)g_process.pid, 0);
    sock->can_be_read = true;
    sock->can_be_written = true;
    return 0;
}

static int bind(struct libos_handle* handle, void* addr, size_t addrlen) {
    struct libos_sock_handle* sock = &handle->info.sock;
    struct sockaddr_nl nl;
    if (addrlen < sizeof(nl))
        return -EINVAL;
    memcpy(&nl, addr, sizeof(nl));
    if (nl.nl_family != AF_NETLINK)
        return -EINVAL;
    set_local_addr(sock, nl.nl_pid ? nl.nl_pid : (uint32_t)g_process.pid, nl.nl_groups);
    sock->was_bound = true;
    return 0;
}

static int listen(struct libos_handle* handle, unsigned int backlog) {
    __UNUSED(handle);
    __UNUSED(backlog);
    return -EOPNOTSUPP;
}

static int accept(struct libos_handle* handle, bool is_nonblocking,
                  struct libos_handle** out_client) {
    __UNUSED(handle);
    __UNUSED(is_nonblocking);
    __UNUSED(out_client);
    return -EOPNOTSUPP;
}

static int connect(struct libos_handle* handle, void* addr, size_t addrlen, bool* out_inprogress) {
    __UNUSED(handle);
    __UNUSED(addr);
    __UNUSED(addrlen);
    __UNUSED(out_inprogress);
    return -EOPNOTSUPP;
}

static int disconnect(struct libos_handle* handle) {
    __UNUSED(handle);
    return -EOPNOTSUPP;
}

static int getsockopt(struct libos_handle* handle, int level, int optname, void* optval,
                      size_t* len) {
    __UNUSED(handle);
    __UNUSED(level);
    __UNUSED(optname);
    __UNUSED(optval);
    __UNUSED(len);
    return -ENOPROTOOPT;
}

static int setsockopt(struct libos_handle* handle, int level, int optname, void* optval,
                      size_t len) {
    __UNUSED(handle);
    __UNUSED(optname);
    __UNUSED(optval);
    __UNUSED(len);
    /* SOL_NETLINK options (NETLINK_EXT_ACK, NETLINK_GET_STRICT_CHK...) are accepted and ignored */
    return level == SOL_NETLINK ? 0 : -ENOPROTOOPT;
}

/* --- reply construction: a buffer of REPLY_MAX bytes, nlmsg after nlmsg --- */

struct reply {
    char* buf;
    size_t len;
    uint32_t seq;
    uint32_t pid;
};

static struct nlmsghdr* reply_msg(struct reply* r, uint16_t type, uint16_t flags, size_t payload) {
    size_t len = NLMSG_LENGTH(payload);
    if (r->len + NLMSG_ALIGN(len) > REPLY_MAX)
        return NULL;
    struct nlmsghdr* nlh = (struct nlmsghdr*)(r->buf + r->len);
    memset(nlh, 0, NLMSG_ALIGN(len));
    nlh->nlmsg_len = len;
    nlh->nlmsg_type = type;
    nlh->nlmsg_flags = flags;
    nlh->nlmsg_seq = r->seq;
    nlh->nlmsg_pid = r->pid;
    r->len += NLMSG_ALIGN(len);
    return nlh;
}

/* appends an attribute to the last message of `r` (the message must be the last one) */
static bool reply_attr(struct reply* r, struct nlmsghdr* nlh, uint16_t type, const void* data,
                       size_t size) {
    size_t at = NLMSG_ALIGN(nlh->nlmsg_len);
    size_t len = RTA_LENGTH(size);
    if ((char*)nlh + at + RTA_ALIGN(len) > r->buf + REPLY_MAX)
        return false;
    struct rtattr* rta = (struct rtattr*)((char*)nlh + at);
    memset(rta, 0, RTA_ALIGN(len));
    rta->rta_type = type;
    rta->rta_len = len;
    memcpy(RTA_DATA(rta), data, size);
    nlh->nlmsg_len = at + len;
    r->len = (char*)nlh + NLMSG_ALIGN(nlh->nlmsg_len) - r->buf;
    return true;
}

static bool reply_done(struct reply* r) {
    struct nlmsghdr* nlh = reply_msg(r, NLMSG_DONE, NLM_F_MULTI, sizeof(int));
    return nlh != NULL;
}

static bool reply_error(struct reply* r, int error, const struct nlmsghdr* req) {
    struct nlmsghdr* nlh = reply_msg(r, NLMSG_ERROR, 0, sizeof(struct nlmsgerr));
    if (!nlh)
        return false;
    struct nlmsgerr* e = NLMSG_DATA(nlh);
    e->error = error;
    e->msg = *req;
    return true;
}

static bool reply_link_lo(struct reply* r) {
    struct nlmsghdr* nlh = reply_msg(r, RTM_NEWLINK, NLM_F_MULTI, sizeof(struct ifinfomsg));
    if (!nlh)
        return false;
    struct ifinfomsg* ifi = NLMSG_DATA(nlh);
    ifi->ifi_family = AF_UNSPEC;
    ifi->ifi_type = NL_ARPHRD_LOOPBACK;
    ifi->ifi_index = LO_INDEX;
    ifi->ifi_flags = NL_IFF_UP | NL_IFF_LOOPBACK | NL_IFF_RUNNING | NL_IFF_LOWER_UP;
    ifi->ifi_change = 0xffffffff;
    static const char name[] = "lo";
    static const uint8_t hwaddr[6] = {0};
    uint32_t mtu = LO_MTU;
    uint8_t operstate = 0; /* IF_OPER_UNKNOWN, as the kernel reports for lo */
    return reply_attr(r, nlh, IFLA_IFNAME, name, sizeof(name)) &&
           reply_attr(r, nlh, IFLA_ADDRESS, hwaddr, sizeof(hwaddr)) &&
           reply_attr(r, nlh, IFLA_BROADCAST, hwaddr, sizeof(hwaddr)) &&
           reply_attr(r, nlh, IFLA_MTU, &mtu, sizeof(mtu)) &&
           reply_attr(r, nlh, IFLA_OPERSTATE, &operstate, sizeof(operstate));
}

static bool reply_addr_lo(struct reply* r, int family) {
    struct nlmsghdr* nlh = reply_msg(r, RTM_NEWADDR, NLM_F_MULTI, sizeof(struct ifaddrmsg));
    if (!nlh)
        return false;
    struct ifaddrmsg* ifa = NLMSG_DATA(nlh);
    ifa->ifa_family = family;
    ifa->ifa_prefixlen = family == AF_INET ? 8 : 128;
    ifa->ifa_flags = IFA_F_PERMANENT;
    ifa->ifa_scope = RT_SCOPE_HOST;
    ifa->ifa_index = LO_INDEX;
    if (family == AF_INET) {
        static const uint8_t v4[4] = {127, 0, 0, 1};
        static const char label[] = "lo";
        return reply_attr(r, nlh, IFA_ADDRESS, v4, sizeof(v4)) &&
               reply_attr(r, nlh, IFA_LOCAL, v4, sizeof(v4)) &&
               reply_attr(r, nlh, IFA_LABEL, label, sizeof(label));
    }
    static const uint8_t v6[16] = {[15] = 1};
    return reply_attr(r, nlh, IFA_ADDRESS, v6, sizeof(v6));
}

/* builds the reply to one request; returns the reply's size, 0 if the request is not one we
 * answer (it is then dropped, as the kernel ignores messages without NLM_F_REQUEST) */
static size_t build_reply(struct libos_sock_handle* sock, const char* req, size_t req_len,
                          char* out) {
    struct nlmsghdr nlh;
    if (req_len < sizeof(nlh))
        return 0;
    memcpy(&nlh, req, sizeof(nlh));
    if (nlh.nlmsg_len < sizeof(nlh) || nlh.nlmsg_len > req_len || !(nlh.nlmsg_flags & NLM_F_REQUEST))
        return 0;
    struct reply r = {.buf = out, .len = 0, .seq = nlh.nlmsg_seq, .pid = netlink_port(sock)};
    bool ok;
    switch (nlh.nlmsg_type) {
        case RTM_GETLINK:
            ok = (!(nlh.nlmsg_flags & NLM_F_DUMP) || reply_link_lo(&r)) && reply_done(&r);
            if (!(nlh.nlmsg_flags & NLM_F_DUMP)) {
                /* a single link request (ifinfomsg + IFLA_IFNAME/ifi_index): only `lo` exists */
                r.len = 0;
                ok = reply_link_lo(&r);
            }
            break;
        case RTM_GETADDR: {
            int family = AF_UNSPEC;
            if (nlh.nlmsg_len >= NLMSG_LENGTH(sizeof(struct rtgenmsg)))
                family = ((const struct rtgenmsg*)(req + NLMSG_HDRLEN))->rtgen_family;
            ok = true;
            if (family == AF_UNSPEC || family == AF_INET)
                ok = ok && reply_addr_lo(&r, AF_INET);
            if (family == AF_UNSPEC || family == AF_INET6)
                ok = ok && reply_addr_lo(&r, AF_INET6);
            ok = ok && reply_done(&r);
            break;
        }
        default:
            if (FIRST_TIME())
                log_debug("netlink: request type %u is not emulated (EOPNOTSUPP)", nlh.nlmsg_type);
            r.len = 0;
            ok = reply_error(&r, -EOPNOTSUPP, &nlh);
            break;
    }
    if (!ok && (nlh.nlmsg_flags & NLM_F_ACK) == 0 && r.len == 0)
        return 0;
    return r.len;
}

static int send(struct libos_handle* handle, struct iovec* iov, size_t iov_len, void* msg_control,
                size_t msg_controllen, size_t* out_size, void* addr, size_t addrlen,
                bool force_nonblocking) {
    __UNUSED(msg_control);
    __UNUSED(msg_controllen);
    __UNUSED(addr);
    __UNUSED(addrlen);
    __UNUSED(force_nonblocking);
    struct libos_sock_handle* sock = &handle->info.sock;

    char req[REPLY_MAX];
    size_t req_len = 0;
    size_t total = 0;
    for (size_t i = 0; i < iov_len; i++) {
        size_t n = MIN(iov[i].iov_len, sizeof(req) - req_len);
        memcpy(req + req_len, iov[i].iov_base, n);
        req_len += n;
        total += iov[i].iov_len;
    }

    char* reply = malloc(REPLY_MAX);
    if (!reply)
        return -ENOMEM;
    size_t reply_len = build_reply(sock, req, req_len, reply);
    lock(&sock->recv_lock);
    free(sock->peek.buf); /* an unread reply of an earlier request is dropped */
    sock->peek.buf = reply_len ? reply : NULL;
    sock->peek.buf_size = reply_len ? REPLY_MAX : 0;
    sock->peek.data_size = reply_len;
    unlock(&sock->recv_lock);
    if (!reply_len)
        free(reply);
    *out_size = total;
    return 0;
}

static int recv(struct libos_handle* handle, struct iovec* iov, size_t iov_len, void* msg_control,
                size_t* msg_controllen_ptr, size_t* out_total_size, void* addr, size_t* addrlen_ptr,
                bool force_nonblocking, bool cmsg_cloexec) {
    __UNUSED(msg_control);
    __UNUSED(force_nonblocking);
    __UNUSED(cmsg_cloexec);
    struct libos_sock_handle* sock = &handle->info.sock;
    if (msg_controllen_ptr)
        *msg_controllen_ptr = 0;

    lock(&sock->recv_lock);
    char* reply = sock->peek.buf;
    size_t reply_len = sock->peek.data_size;
    sock->peek.buf = NULL;
    sock->peek.buf_size = 0;
    sock->peek.data_size = 0;
    unlock(&sock->recv_lock);
    if (!reply) {
        /* nothing was requested: a kernel socket would block; we never deliver anything later */
        return -EAGAIN;
    }
    size_t copied = 0;
    for (size_t i = 0; i < iov_len && copied < reply_len; i++) {
        size_t n = MIN(iov[i].iov_len, reply_len - copied);
        memcpy(iov[i].iov_base, reply + copied, n);
        copied += n;
    }
    free(reply);
    if (addr && addrlen_ptr) {
        struct sockaddr_nl nl = {.nl_family = AF_NETLINK, .nl_pid = 0 /* the kernel */};
        size_t n = MIN(*addrlen_ptr, sizeof(nl));
        memcpy(addr, &nl, n);
        *addrlen_ptr = sizeof(nl);
    }
    *out_total_size = reply_len;
    return 0;
}

static int register_shmem(struct libos_handle* handle, void* region) {
    __UNUSED(handle);
    __UNUSED(region);
    return -ENOSYS;
}

struct libos_sock_ops sock_netlink_ops = {
    .create = create,
    .bind = bind,
    .listen = listen,
    .accept = accept,
    .connect = connect,
    .disconnect = disconnect,
    .getsockopt = getsockopt,
    .setsockopt = setsockopt,
    .send = send,
    .recv = recv,
    .register_shmem = register_shmem,
};
