/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2022 Intel Corporation
 *                    Borys Popławski <borysp@invisiblethingslab.com>
 * Copyright (C) 2026 The ASTERISC Research Lab
 */

/*
 * TCP and UDP sockets of the vbs PAL proper: sockets of the untrusted host (Winsock), reached
 * through host requests (pal_hostio.c, PAL_VBS_HOST_* in pal_vbs_abi.h). Adapted from the Linux
 * PAL's pal_sockets.c; the host speaks Linux sockaddr layouts and errno values. As in the SGX PAL,
 * the network is untrusted: the application must protect its data itself (TLS).
 */

#include "api.h"
#include "linux_socket.h"
#include "pal.h"
#include "pal_error.h"
#include "pal_internal.h"
#include "pal_linux_error.h"
#include "pal_vbs.h"
#include "socket_utils.h"

#define L_SOCK_STREAM 1
#define L_SOCK_DGRAM  2
#define L_SOL_SOCKET  1
#define L_SO_REUSEADDR 2
#define L_SO_BROADCAST 6
#define L_SO_SNDBUF   7
#define L_SO_RCVBUF   8
#define L_SO_KEEPALIVE 9
#define L_IPPROTO_TCP 6
#define L_TCP_NODELAY 1
#define L_IPPROTO_IPV6 41
#define L_IPV6_V6ONLY 26
#define L_EINPROGRESS 115

#define DEFAULT_BUF_SIZE (64 * 1024)

static struct handle_ops g_tcp_handle_ops;
static struct handle_ops g_udp_handle_ops;
static struct socket_ops g_tcp_sock_ops;
static struct socket_ops g_udp_sock_ops;

static int64_t host(uint64_t op, int64_t a0, int64_t a1, int64_t a2, int64_t a3, const void* in,
                    size_t in_len, void* out, size_t out_cap, size_t* out_len) {
    return vbs_hostcall(op, a0, a1, a2, a3, in, in_len, out, out_cap, out_len);
}

static PAL_HANDLE create_sock_handle(int64_t id, enum pal_socket_domain domain,
                                     enum pal_socket_type type, struct handle_ops* handle_ops,
                                     struct socket_ops* ops, bool is_nonblocking) {
    PAL_HANDLE handle = calloc(1, sizeof(*handle));
    if (!handle)
        return NULL;
    handle->hdr.type = PAL_TYPE_SOCKET;
    handle->hdr.ops = handle_ops;
    handle->sock.host_id = id;
    handle->sock.domain = domain;
    handle->sock.type = type;
    handle->sock.ops = ops;
    handle->sock.recv_buf_size = DEFAULT_BUF_SIZE;
    handle->sock.send_buf_size = DEFAULT_BUF_SIZE;
    handle->sock.is_nonblocking = is_nonblocking;
    handle->sock.tcp_keepidle = DEFAULT_TCP_KEEPIDLE;
    handle->sock.tcp_keepintvl = DEFAULT_TCP_KEEPINTVL;
    handle->sock.tcp_keepcnt = DEFAULT_TCP_KEEPCNT;
    handle->sock.tcp_user_timeout = DEFAULT_TCP_USER_TIMEOUT;
    return handle;
}

int _PalSocketCreate(enum pal_socket_domain domain, enum pal_socket_type type,
                     pal_stream_options_t options, PAL_HANDLE* out_handle) {
    int linux_domain, linux_type;
    switch (domain) {
        case PAL_IPV4: linux_domain = AF_INET; break;
        case PAL_IPV6: linux_domain = AF_INET6; break;
        default: return -PAL_ERROR_NOTSUPPORT;
    }
    struct handle_ops* handle_ops;
    struct socket_ops* sock_ops;
    switch (type) {
        case PAL_SOCKET_TCP:
            linux_type = L_SOCK_STREAM;
            handle_ops = &g_tcp_handle_ops;
            sock_ops = &g_tcp_sock_ops;
            break;
        case PAL_SOCKET_UDP:
            linux_type = L_SOCK_DGRAM;
            handle_ops = &g_udp_handle_ops;
            sock_ops = &g_udp_sock_ops;
            break;
        default:
            return -PAL_ERROR_NOTSUPPORT;
    }
    bool nonblock = !!(options & PAL_OPTION_NONBLOCK);
    int64_t id = host(PAL_VBS_HOST_SOCKET, linux_domain, linux_type, nonblock, 0, NULL, 0, NULL,
                      0, NULL);
    if (id < 0)
        return unix_to_pal_error((int)id);
    PAL_HANDLE handle = create_sock_handle(id, domain, type, handle_ops, sock_ops, nonblock);
    if (!handle) {
        host(PAL_VBS_HOST_CLOSE, id, 0, 0, 0, NULL, 0, NULL, 0, NULL);
        return -PAL_ERROR_NOMEM;
    }
    *out_handle = handle;
    return 0;
}

static void destroy(PAL_HANDLE handle) {
    host(PAL_VBS_HOST_CLOSE, handle->sock.host_id, 0, 0, 0, NULL, 0, NULL, 0, NULL);
    free(handle);
}

/* a sockaddr from the host: check its family against the socket's */
static int host_sockaddr(const struct sockaddr_storage* ss, size_t len, enum pal_socket_domain dom,
                         struct pal_socket_addr* out) {
    if (len < sizeof(struct sockaddr_in))
        return -PAL_ERROR_DENIED;
    if ((dom == PAL_IPV4 && ss->ss_family != AF_INET) ||
            (dom == PAL_IPV6 && (ss->ss_family != AF_INET6 || len < sizeof(struct sockaddr_in6))))
        return -PAL_ERROR_DENIED;
    linux_to_pal_sockaddr(ss, out);
    return 0;
}

static int bind(PAL_HANDLE handle, struct pal_socket_addr* addr) {
    if (addr->domain != handle->sock.domain)
        return -PAL_ERROR_INVAL;
    struct sockaddr_storage ss;
    size_t len;
    pal_to_linux_sockaddr(addr, &ss, &len);
    struct sockaddr_storage bound;
    memset(&bound, 0, sizeof(bound));
    size_t blen = 0;
    int64_t r = host(PAL_VBS_HOST_BIND, handle->sock.host_id, 0, 0, 0, &ss, len, &bound,
                     sizeof(bound), &blen);
    if (r < 0)
        return unix_to_pal_error((int)r);
    struct pal_socket_addr got;
    if (host_sockaddr(&bound, blen, handle->sock.domain, &got) == 0) {
        if (addr->domain == PAL_IPV4 && !addr->ipv4.port)
            addr->ipv4.port = got.ipv4.port;
        else if (addr->domain == PAL_IPV6 && !addr->ipv6.port)
            addr->ipv6.port = got.ipv6.port;
    }
    return 0;
}

static int tcp_listen(PAL_HANDLE handle, unsigned int backlog) {
    int64_t r = host(PAL_VBS_HOST_LISTEN, handle->sock.host_id, backlog, 0, 0, NULL, 0, NULL, 0,
                     NULL);
    return r < 0 ? unix_to_pal_error((int)r) : 0;
}

static int getsockname_pal(int64_t id, enum pal_socket_domain dom, struct pal_socket_addr* out) {
    struct sockaddr_storage ss;
    memset(&ss, 0, sizeof(ss));
    size_t len = 0;
    int64_t r = host(PAL_VBS_HOST_GETSOCKNAME, id, 0, 0, 0, NULL, 0, &ss, sizeof(ss), &len);
    if (r < 0)
        return unix_to_pal_error((int)r);
    return host_sockaddr(&ss, len, dom, out);
}

static int tcp_accept(PAL_HANDLE handle, pal_stream_options_t options, PAL_HANDLE* out_client,
                      struct pal_socket_addr* out_client_addr,
                      struct pal_socket_addr* out_local_addr) {
    bool nonblock = !!(options & PAL_OPTION_NONBLOCK);
    struct sockaddr_storage peer;
    memset(&peer, 0, sizeof(peer));
    size_t plen = 0;
    int64_t id = host(PAL_VBS_HOST_ACCEPT, handle->sock.host_id, nonblock, 0, 0, NULL, 0, &peer,
                      sizeof(peer), &plen);
    if (id < 0)
        return unix_to_pal_error((int)id);
    PAL_HANDLE client = create_sock_handle(id, handle->sock.domain, handle->sock.type,
                                           handle->hdr.ops, handle->sock.ops, nonblock);
    if (!client) {
        host(PAL_VBS_HOST_CLOSE, id, 0, 0, 0, NULL, 0, NULL, 0, NULL);
        return -PAL_ERROR_NOMEM;
    }
    if (out_local_addr) {
        int ret = getsockname_pal(id, client->sock.domain, out_local_addr);
        if (ret < 0) {
            _PalObjectDestroy(client);
            return ret;
        }
    }
    if (out_client_addr) {
        int ret = host_sockaddr(&peer, plen, client->sock.domain, out_client_addr);
        if (ret < 0) {
            _PalObjectDestroy(client);
            return ret;
        }
    }
    *out_client = client;
    return 0;
}

static int connect(PAL_HANDLE handle, struct pal_socket_addr* addr,
                   struct pal_socket_addr* out_local_addr, bool* out_inprogress) {
    if (addr->domain == PAL_DISCONNECT)
        return -PAL_ERROR_NOTSUPPORT;
    if (addr->domain != handle->sock.domain)
        return -PAL_ERROR_INVAL;
    struct sockaddr_storage ss;
    size_t len;
    pal_to_linux_sockaddr(addr, &ss, &len);
    struct sockaddr_storage local;
    memset(&local, 0, sizeof(local));
    size_t llen = 0;
    int64_t r = host(PAL_VBS_HOST_CONNECT, handle->sock.host_id, 0, 0, 0, &ss, len, &local,
                     sizeof(local), &llen);
    if (r < 0 && r != -L_EINPROGRESS)
        return unix_to_pal_error((int)r);
    if (out_local_addr) {
        int ret = host_sockaddr(&local, llen, handle->sock.domain, out_local_addr);
        if (ret < 0)
            return ret;
    }
    *out_inprogress = (r == -L_EINPROGRESS);
    return 0;
}

static int attrquerybyhdl(PAL_HANDLE handle, PAL_STREAM_ATTR* attr) {
    memset(attr, 0, sizeof(*attr));
    attr->handle_type = PAL_TYPE_SOCKET;
    attr->nonblocking = handle->sock.is_nonblocking;
    int64_t pending = host(PAL_VBS_HOST_FIONREAD, handle->sock.host_id, 0, 0, 0, NULL, 0, NULL, 0,
                           NULL);
    attr->pending_size = pending > 0 ? (size_t)pending : 0;
    attr->socket.linger = handle->sock.linger;
    attr->socket.recv_buf_size = handle->sock.recv_buf_size;
    attr->socket.send_buf_size = handle->sock.send_buf_size;
    attr->socket.receivetimeout_us = handle->sock.recvtimeout_us;
    attr->socket.sendtimeout_us = handle->sock.sendtimeout_us;
    attr->socket.reuseaddr = handle->sock.reuseaddr;
    attr->socket.reuseport = handle->sock.reuseport;
    attr->socket.broadcast = handle->sock.broadcast;
    attr->socket.keepalive = handle->sock.keepalive;
    attr->socket.tcp_cork = handle->sock.tcp_cork;
    attr->socket.tcp_keepidle = handle->sock.tcp_keepidle;
    attr->socket.tcp_keepintvl = handle->sock.tcp_keepintvl;
    attr->socket.tcp_keepcnt = handle->sock.tcp_keepcnt;
    attr->socket.tcp_nodelay = handle->sock.tcp_nodelay;
    attr->socket.tcp_user_timeout = handle->sock.tcp_user_timeout;
    attr->socket.ipv6_v6only = handle->sock.ipv6_v6only;
    return 0;
}

static int setopt(PAL_HANDLE handle, int64_t level, int64_t name, int64_t value) {
    int64_t r = host(PAL_VBS_HOST_SETSOCKOPT, handle->sock.host_id, level, name, value, NULL, 0,
                     NULL, 0, NULL);
    return r < 0 ? unix_to_pal_error((int)r) : 0;
}

static int attrsetbyhdl_common(PAL_HANDLE handle, PAL_STREAM_ATTR* attr) {
    if (attr->handle_type != PAL_TYPE_SOCKET)
        return -PAL_ERROR_INVAL;
    int ret;
    if (attr->nonblocking != handle->sock.is_nonblocking) {
        int64_t r = host(PAL_VBS_HOST_SETNONBLOCK, handle->sock.host_id, attr->nonblocking, 0, 0,
                         NULL, 0, NULL, 0, NULL);
        if (r < 0)
            return unix_to_pal_error((int)r);
        handle->sock.is_nonblocking = attr->nonblocking;
    }
    /* kept by the PAL (reported back, not enforced on the host): linger, timeouts, reuseport */
    handle->sock.linger = attr->socket.linger;
    handle->sock.recvtimeout_us = attr->socket.receivetimeout_us;
    handle->sock.sendtimeout_us = attr->socket.sendtimeout_us;
    handle->sock.reuseport = attr->socket.reuseport;
    if (attr->socket.recv_buf_size != handle->sock.recv_buf_size) {
        if ((ret = setopt(handle, L_SOL_SOCKET, L_SO_RCVBUF, attr->socket.recv_buf_size)) < 0)
            return ret;
        handle->sock.recv_buf_size = attr->socket.recv_buf_size;
    }
    if (attr->socket.send_buf_size != handle->sock.send_buf_size) {
        if ((ret = setopt(handle, L_SOL_SOCKET, L_SO_SNDBUF, attr->socket.send_buf_size)) < 0)
            return ret;
        handle->sock.send_buf_size = attr->socket.send_buf_size;
    }
    if (attr->socket.keepalive != handle->sock.keepalive) {
        if ((ret = setopt(handle, L_SOL_SOCKET, L_SO_KEEPALIVE, attr->socket.keepalive)) < 0)
            return ret;
        handle->sock.keepalive = attr->socket.keepalive;
    }
    if (attr->socket.reuseaddr != handle->sock.reuseaddr) {
        if ((ret = setopt(handle, L_SOL_SOCKET, L_SO_REUSEADDR, attr->socket.reuseaddr)) < 0)
            return ret;
        handle->sock.reuseaddr = attr->socket.reuseaddr;
    }
    if (attr->socket.broadcast != handle->sock.broadcast) {
        if ((ret = setopt(handle, L_SOL_SOCKET, L_SO_BROADCAST, attr->socket.broadcast)) < 0)
            return ret;
        handle->sock.broadcast = attr->socket.broadcast;
    }
    if (attr->socket.ipv6_v6only != handle->sock.ipv6_v6only) {
        if ((ret = setopt(handle, L_IPPROTO_IPV6, L_IPV6_V6ONLY, attr->socket.ipv6_v6only)) < 0)
            return ret;
        handle->sock.ipv6_v6only = attr->socket.ipv6_v6only;
    }
    return 0;
}

static int attrsetbyhdl_tcp(PAL_HANDLE handle, PAL_STREAM_ATTR* attr) {
    int ret = attrsetbyhdl_common(handle, attr);
    if (ret < 0)
        return ret;
    if (attr->socket.tcp_nodelay != handle->sock.tcp_nodelay) {
        if ((ret = setopt(handle, L_IPPROTO_TCP, L_TCP_NODELAY, attr->socket.tcp_nodelay)) < 0)
            return ret;
        handle->sock.tcp_nodelay = attr->socket.tcp_nodelay;
    }
    /* kept by the PAL only */
    handle->sock.tcp_cork = attr->socket.tcp_cork;
    handle->sock.tcp_keepidle = attr->socket.tcp_keepidle;
    handle->sock.tcp_keepintvl = attr->socket.tcp_keepintvl;
    handle->sock.tcp_keepcnt = attr->socket.tcp_keepcnt;
    handle->sock.tcp_user_timeout = attr->socket.tcp_user_timeout;
    return 0;
}

static int attrsetbyhdl_udp(PAL_HANDLE handle, PAL_STREAM_ATTR* attr) {
    return attrsetbyhdl_common(handle, attr);
}

/* Sends and receives go between the application's buffers and the thread's host buffer in one copy
 * (vbs_hostcall_v, scatter/gather), without a staging buffer in the enclave. */
static int send(PAL_HANDLE handle, struct iovec* iov, size_t iov_len, size_t* out_size,
                struct pal_socket_addr* addr, bool force_nonblocking) {
    /* the prefix of the request: [sockaddr length u32][sockaddr] */
    struct {
        uint32_t alen;
        struct sockaddr_storage ss;
    } __attribute__((packed)) pre;
    size_t alen = 0;
    if (addr) {
        if (addr->domain != handle->sock.domain)
            return -PAL_ERROR_INVAL;
        pal_to_linux_sockaddr(addr, &pre.ss, &alen);
    }
    pre.alen = (uint32_t)alen;
    size_t total = 0;
    for (size_t i = 0; i < iov_len; i++)
        total += iov[i].iov_len;
    size_t max = PAL_VBS_HOST_DATA_MAX - 4 - alen;
    if (total > max) {
        if (handle->sock.type == PAL_SOCKET_UDP)
            return -PAL_ERROR_TOOLONG;
        total = max; /* a stream socket may send less (the host request takes what fits) */
    }
    int64_t r = vbs_hostcall_v(PAL_VBS_HOST_SEND, handle->sock.host_id, force_nonblocking,
                               handle->sock.is_nonblocking, 0, &pre, 4 + alen, iov, iov_len, NULL,
                               0, NULL, 0, NULL);
    if (r < 0)
        return unix_to_pal_error((int)r);
    if ((uint64_t)r > total)
        return -PAL_ERROR_DENIED;
    *out_size = (size_t)r;
    return 0;
}

static int recv(PAL_HANDLE handle, struct iovec* iov, size_t iov_len, size_t* out_total_size,
                struct pal_socket_addr* addr, bool force_nonblocking) {
    size_t total = 0;
    for (size_t i = 0; i < iov_len; i++)
        total += iov[i].iov_len;
    total = MIN(total, (size_t)(PAL_VBS_HOST_DATA_MAX - PAL_VBS_HOST_SOCKADDR_MAX));
    /* the host's output: [sockaddr, PAL_VBS_HOST_SOCKADDR_MAX bytes][bytes] */
    struct sockaddr_storage ss;
    _Static_assert(sizeof(ss) == PAL_VBS_HOST_SOCKADDR_MAX, "sockaddr prefix");
    memset(&ss, 0, sizeof(ss));
    size_t got = 0;
    int64_t r = vbs_hostcall_v(PAL_VBS_HOST_RECV, handle->sock.host_id, total, force_nonblocking,
                               handle->sock.is_nonblocking, NULL, 0, NULL, 0, &ss,
                               PAL_VBS_HOST_SOCKADDR_MAX, iov, iov_len, &got);
    if (r < 0)
        return unix_to_pal_error((int)r);
    size_t n = got > PAL_VBS_HOST_SOCKADDR_MAX ? got - PAL_VBS_HOST_SOCKADDR_MAX : 0;
    if (n > total || (handle->sock.type != PAL_SOCKET_UDP && (uint64_t)r != n))
        return -PAL_ERROR_DENIED; /* the host stored more than asked, or lied about the count */
    if (addr) {
        if (host_sockaddr(&ss, sizeof(ss), handle->sock.domain, addr) < 0)
            memset(addr, 0, sizeof(*addr));
    }
    /* UDP reports the full datagram length (PalSocketRecv); the host gave what fit */
    *out_total_size = handle->sock.type == PAL_SOCKET_UDP ? (size_t)r : n;
    return 0;
}

static int delete_tcp(PAL_HANDLE handle, enum pal_delete_mode mode) {
    int how;
    switch (mode) {
        case PAL_DELETE_ALL:   how = 2; break;
        case PAL_DELETE_READ:  how = 0; break;
        case PAL_DELETE_WRITE: how = 1; break;
        default: return -PAL_ERROR_INVAL;
    }
    int64_t r = host(PAL_VBS_HOST_SHUTDOWN, handle->sock.host_id, how, 0, 0, NULL, 0, NULL, 0,
                     NULL);
    return r < 0 ? unix_to_pal_error((int)r) : 0;
}

static int delete_udp(PAL_HANDLE handle, enum pal_delete_mode mode) {
    __UNUSED(handle);
    __UNUSED(mode);
    return 0;
}

static struct socket_ops g_tcp_sock_ops = {
    .bind = bind,
    .listen = tcp_listen,
    .accept = tcp_accept,
    .connect = connect,
    .send = send,
    .recv = recv,
};

static struct socket_ops g_udp_sock_ops = {
    .bind = bind,
    .connect = connect,
    .send = send,
    .recv = recv,
};

static struct handle_ops g_tcp_handle_ops = {
    .attrquerybyhdl = attrquerybyhdl,
    .attrsetbyhdl = attrsetbyhdl_tcp,
    .delete = delete_tcp,
    .destroy = destroy,
};

static struct handle_ops g_udp_handle_ops = {
    .attrquerybyhdl = attrquerybyhdl,
    .attrsetbyhdl = attrsetbyhdl_udp,
    .delete = delete_udp,
    .destroy = destroy,
};

int _PalSocketBind(PAL_HANDLE handle, struct pal_socket_addr* addr) {
    if (!handle->sock.ops->bind)
        return -PAL_ERROR_NOTSUPPORT;
    return handle->sock.ops->bind(handle, addr);
}

int _PalSocketListen(PAL_HANDLE handle, unsigned int backlog) {
    if (!handle->sock.ops->listen)
        return -PAL_ERROR_NOTSUPPORT;
    return handle->sock.ops->listen(handle, backlog);
}

int _PalSocketAccept(PAL_HANDLE handle, pal_stream_options_t options, PAL_HANDLE* out_client,
                     struct pal_socket_addr* out_client_addr,
                     struct pal_socket_addr* out_local_addr) {
    if (!handle->sock.ops->accept)
        return -PAL_ERROR_NOTSUPPORT;
    return handle->sock.ops->accept(handle, options, out_client, out_client_addr, out_local_addr);
}

int _PalSocketConnect(PAL_HANDLE handle, struct pal_socket_addr* addr,
                      struct pal_socket_addr* out_local_addr, bool* out_inprogress) {
    if (!handle->sock.ops->connect)
        return -PAL_ERROR_NOTSUPPORT;
    return handle->sock.ops->connect(handle, addr, out_local_addr, out_inprogress);
}

int _PalSocketSend(PAL_HANDLE handle, struct iovec* iov, size_t iov_len, size_t* out_size,
                   struct pal_socket_addr* addr, bool force_nonblocking) {
    if (!handle->sock.ops->send)
        return -PAL_ERROR_NOTSUPPORT;
    return handle->sock.ops->send(handle, iov, iov_len, out_size, addr, force_nonblocking);
}

int _PalSocketRecv(PAL_HANDLE handle, struct iovec* iov, size_t iov_len, size_t* out_total_size,
                   struct pal_socket_addr* addr, bool force_nonblocking) {
    if (!handle->sock.ops->recv)
        return -PAL_ERROR_NOTSUPPORT;
    return handle->sock.ops->recv(handle, iov, iov_len, out_total_size, addr, force_nonblocking);
}

int _PalSocketRegisterShmem(PAL_HANDLE handle, void* region) {
    __UNUSED(handle);
    __UNUSED(region);
    return -PAL_ERROR_NOTIMPLEMENTED;
}
