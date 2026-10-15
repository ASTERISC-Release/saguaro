/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2023 Intel Corporation */

/*
 * TCP/UDP sockets, emulated through AF_VSOCK. Notes:
 *   - bound/connected IP addresses are dummy and emulated as localhost,
 *   - UDP sockets are not really supported, they have no send() and recv() callbacks.
 */

#include "api.h"
#include "linux_socket.h"
#include "list.h"
#include "pal.h"
#include "pal_common.h"
#include "pal_error.h"
#include "pal_linux_error.h"
#include "pal_internal.h"
#include "socket_utils.h"
#include "spinlock.h"
#include "toml.h"

#include "kernel_sched.h"
#include "kernel_time.h"
#include "kernel_virtio.h"
#include "kernel_virtio_vsock.h"
#include <stdatomic.h>

static struct handle_ops g_tcp_handle_ops;
static struct handle_ops g_udp_handle_ops;
static struct socket_ops g_tcp_sock_ops;
static struct socket_ops g_udp_sock_ops;
struct sock_to_region_maps {
    uint32_t port_num;
    struct pal_handle* region_handle;
} SOCK_REGION_MAP[100];

static int current_indx_in_sock_region_map = 0;
static spinlock_t g_sock_region_map_lock = INIT_SPINLOCK_UNLOCKED;
/* Default values on a modern Linux kernel. */
static size_t g_default_recv_buf_size = 0x20000;
static size_t g_default_send_buf_size = 0x4000;

/* RX/TX virtqueue events (see virtio-vsock.c) trigger the corresponding futex; we use global
 * futexes instead of per-socket ones for simplicity (it is cumbersome to associate each
 * received/sent network packet to the socket PAL-handle object) and because these events are
 * sufficiently rare and may have sub-optimal performance */
static int g_sockets_reader_futex;
static int g_sockets_writer_futex;
static int udp_rx_futex;
void thread_wakeup_vsock(bool is_read) {
    sched_thread_wakeup(&g_streams_waiting_events_futex);
    sched_thread_wakeup(is_read ? &g_sockets_reader_futex : &g_sockets_writer_futex);
}

void sockets_kick_waiting_threads(void) {
    /* See the declaration in kernel_virtio_vsock.h: a cheap safety net against the wakeup race;
     * skip entirely when there is no vsock device (nothing could be waiting on these). */
    if (!g_vsock)
        return;
    sched_thread_wakeup_uninterruptable(&g_sockets_reader_futex);
    sched_thread_wakeup_uninterruptable(&g_sockets_writer_futex);
}
void udp_rx_ring_init(struct pal_handle *handle, void *region)
{
    struct udp_rx_ring* udp_region = malloc(sizeof(struct udp_rx_ring));
    assert(udp_region != NULL);
    handle->region = udp_region;
    memset(udp_region, 0, sizeof(*udp_region));
    // Not strictly required, but good hygiene
    udp_region->ring = region;
    memset(udp_region->ring, 0, DATA_SIZE);
    udp_region->cap = DATA_SIZE;

}
static inline uint8_t *ring_data(struct udp_rx_ring *r) {
    return r->ring;
}

static bool vm_is_direct_vsock_port(uint16_t port) {
    toml_table_t* vm = toml_table_in(g_pal_public_state.manifest_root, "vm");
    toml_table_t* net = vm ? toml_table_in(vm, "net") : NULL;
    toml_array_t* ports = net ? toml_array_in(net, "direct_vsock_ports") : NULL;
    if (!ports)
        return false;

    int count = toml_array_nelem(ports);
    for (int i = 0; i < count; i++) {
        toml_raw_t raw = toml_raw_at(ports, i);
        int64_t direct_port;
        if (raw && toml_rtoi(raw, &direct_port) == 0 && direct_port == port)
            return true;
    }
    return false;
}

static int vm_relay_write_exact(struct pal_handle* handle, const void* buf, size_t size) {
    size_t offset = 0;
    while (offset < size) {
        struct iovec iov = {
            .iov_base = (char*)buf + offset,
            .iov_len = size - offset,
        };
        size_t written = 0;
        int ret = handle->sock.ops->send(handle, &iov, 1, &written, NULL,
                                         /*force_nonblocking=*/false);
        if (ret < 0)
            return ret;
        if (!written || written > size - offset)
            return -PAL_ERROR_CONNFAILED;
        offset += written;
    }
    return 0;
}

static int vm_relay_read_exact(struct pal_handle* handle, void* buf, size_t size) {
    size_t offset = 0;
    while (offset < size) {
        struct iovec iov = {
            .iov_base = (char*)buf + offset,
            .iov_len = size - offset,
        };
        size_t received = 0;
        int ret = handle->sock.ops->recv(handle, &iov, 1, &received, NULL,
                                         /*force_nonblocking=*/false);
        if (ret < 0)
            return ret;
        if (!received || received > size - offset)
            return -PAL_ERROR_CONNFAILED;
        offset += received;
    }
    return 0;
}

static int vm_relay_connect(struct pal_handle* handle, const struct pal_socket_addr* addr) {
    uint8_t request[12 + 16] = { 'G', 'N', 'R', '1', 1, 1, 6, 0 };
    const uint8_t* address;
    size_t address_size;
    uint16_t port;

    if (addr->domain == PAL_IPV4) {
        request[7] = 4;
        address = (const uint8_t*)&addr->ipv4.addr;
        address_size = 4;
        port = ntohs(addr->ipv4.port);
    } else if (addr->domain == PAL_IPV6) {
        request[7] = 6;
        address = addr->ipv6.addr;
        address_size = 16;
        port = ntohs(addr->ipv6.port);
    } else {
        return -PAL_ERROR_AFNOSUPPORT;
    }

    if (!port)
        return -PAL_ERROR_INVAL;
    request[8] = port >> 8;
    request[9] = port & 0xff;
    request[10] = address_size >> 8;
    request[11] = address_size & 0xff;
    memcpy(request + 12, address, address_size);

    /* Connect() is synchronous in the VM PAL, including for non-blocking sockets. Make the short
     * relay handshake use the same blocking send/receive path before restoring the socket flag. */
    spinlock_lock(&handle->sock.lock);
    bool was_nonblocking = handle->sock.is_nonblocking;
    handle->sock.is_nonblocking = false;
    spinlock_unlock(&handle->sock.lock);
    int ret = vm_relay_write_exact(handle, request, 12 + address_size);
    uint8_t response[8];
    if (ret == 0)
        ret = vm_relay_read_exact(handle, response, sizeof(response));
    spinlock_lock(&handle->sock.lock);
    handle->sock.is_nonblocking = was_nonblocking;
    spinlock_unlock(&handle->sock.lock);
    if (ret < 0)
        return ret;

    if (memcmp(response, "GNR1", 4) || response[4] != 1)
        return -PAL_ERROR_CONNFAILED;
    uint16_t host_errno = ((uint16_t)response[6] << 8) | response[7];
    if (response[5] == 0 && host_errno == 0)
        return 0;
    if (response[5] != 1 || !host_errno)
        return -PAL_ERROR_CONNFAILED;
    return -unix_to_pal_error(host_errno);
}

static int vm_prepare_relay_connect(struct pal_handle* handle,
                                    const struct pal_socket_addr* addr) {
    uint8_t* request = handle->sock.connect_request;
    const uint8_t* address;
    size_t address_size;
    uint16_t port;

    memset(request, 0, sizeof(handle->sock.connect_request));
    memcpy(request, "GNR1", 4);
    request[4] = 1;
    request[5] = 1;
    request[6] = 6;
    if (addr->domain == PAL_IPV4) {
        request[7] = 4;
        address = (const uint8_t*)&addr->ipv4.addr;
        address_size = 4;
        port = ntohs(addr->ipv4.port);
    } else if (addr->domain == PAL_IPV6) {
        request[7] = 6;
        address = addr->ipv6.addr;
        address_size = 16;
        port = ntohs(addr->ipv6.port);
    } else {
        return -PAL_ERROR_AFNOSUPPORT;
    }
    if (!port)
        return -PAL_ERROR_INVAL;

    request[8] = port >> 8;
    request[9] = port & 0xff;
    request[10] = address_size >> 8;
    request[11] = address_size & 0xff;
    memcpy(request + 12, address, address_size);
    handle->sock.connect_request_size = 12 + address_size;
    handle->sock.connect_request_offset = 0;
    handle->sock.connect_response_offset = 0;
    return 0;
}

/* Advance a nonblocking connect without sleeping. The stream response is the relay's TCP
 * connect result; once received, normal socket polling reports completion to LibOS. */
static void vm_tcp_connect_progress(struct pal_handle* handle) {
    spinlock_lock(&handle->sock.lock);
    if (!handle->sock.connect_pending) {
        spinlock_unlock(&handle->sock.lock);
        return;
    }

    int ret = virtio_vsock_connect_status(handle->sock.fd);
    if (ret == -PAL_ERROR_TRYAGAIN) {
        spinlock_unlock(&handle->sock.lock);
        return;
    }
    if (ret < 0) {
        handle->sock.connect_result = ret;
        goto complete;
    }

    if (!handle->sock.connect_direct_vsock) {
        while (handle->sock.connect_request_offset < handle->sock.connect_request_size) {
            size_t offset = handle->sock.connect_request_offset;
            size_t remaining = handle->sock.connect_request_size - offset;
            long written = virtio_vsock_write(handle->sock.fd,
                                               handle->sock.connect_request + offset, remaining);
            if (written == -PAL_ERROR_TRYAGAIN) {
                spinlock_unlock(&handle->sock.lock);
                return;
            }
            if (written <= 0 || (size_t)written > remaining) {
                handle->sock.connect_result = written < 0 ? (int)written : -PAL_ERROR_CONNFAILED;
                goto complete;
            }
            handle->sock.connect_request_offset += written;
        }

        while (handle->sock.connect_response_offset < sizeof(handle->sock.connect_response)) {
            size_t offset = handle->sock.connect_response_offset;
            size_t remaining = sizeof(handle->sock.connect_response) - offset;
            long received = virtio_vsock_read(handle->sock.fd,
                                               handle->sock.connect_response + offset, remaining);
            if (received == -PAL_ERROR_TRYAGAIN) {
                spinlock_unlock(&handle->sock.lock);
                return;
            }
            if (received <= 0 || (size_t)received > remaining) {
                handle->sock.connect_result = received < 0 ? (int)received : -PAL_ERROR_CONNFAILED;
                goto complete;
            }
            handle->sock.connect_response_offset += received;
        }

        const uint8_t* response = handle->sock.connect_response;
        if (memcmp(response, "GNR1", 4) || response[4] != 1) {
            handle->sock.connect_result = -PAL_ERROR_CONNFAILED;
        } else {
            uint16_t host_errno = ((uint16_t)response[6] << 8) | response[7];
            if (response[5] == 0 && host_errno == 0) {
                handle->sock.connect_result = 0;
            } else if (response[5] == 1 && host_errno) {
                handle->sock.connect_result = -unix_to_pal_error(host_errno);
            } else {
                handle->sock.connect_result = -PAL_ERROR_CONNFAILED;
            }
        }
    } else {
        handle->sock.connect_result = 0;
    }

complete:
    handle->sock.connect_pending = false;
    handle->sock.connect_finished = true;
    spinlock_unlock(&handle->sock.lock);
}

/* UDP shares one VSOCK stream with the relay; these helpers preserve datagram framing above it. */
static int vm_vsock_write_exact(struct pal_handle* handle, const void* buf, size_t size,
                                bool force_nonblocking) {
    size_t offset = 0;
    while (offset < size) {
        int64_t written = virtio_vsock_write(handle->sock.fd, (const char*)buf + offset,
                                             size - offset);
        if (written == -PAL_ERROR_TRYAGAIN) {
            if (force_nonblocking || handle->sock.is_nonblocking)
                return -PAL_ERROR_TRYAGAIN;
            int wret = sched_thread_wait_interruptible(&g_sockets_writer_futex, &handle->sock.lock);
            if (wret < 0)
                return wret;
            continue;
        }
        if (written < 0)
            return written;
        if (!written || (size_t)written > size - offset)
            return -PAL_ERROR_CONNFAILED;
        offset += (size_t)written;
    }
    return 0;
}

static int vm_vsock_read_exact(struct pal_handle* handle, void* buf, size_t size,
                               bool force_nonblocking) {
    size_t offset = 0;
    while (offset < size) {
        int64_t received = virtio_vsock_read(handle->sock.fd, (char*)buf + offset,
                                             size - offset);
        if (received == -PAL_ERROR_TRYAGAIN) {
            if (force_nonblocking || handle->sock.is_nonblocking)
                return -PAL_ERROR_TRYAGAIN;
            spinlock_unlock(&handle->sock.lock);
            bool got_data = virtio_vsock_busy_poll(/*timeout_us=*/25);
            spinlock_lock(&handle->sock.lock);
            if (got_data)
                continue;
            if (virtio_vsock_rearm_notifications()) {
                spinlock_unlock(&handle->sock.lock);
                (void)virtio_vsock_poll();
                spinlock_lock(&handle->sock.lock);
                continue;
            }
            int wret = sched_thread_wait_interruptible(&g_sockets_reader_futex, &handle->sock.lock);
            if (wret < 0)
                return wret;
            continue;
        }
        if (received < 0)
            return received;
        if (!received || (size_t)received > size - offset)
            return -PAL_ERROR_CONNFAILED;
        offset += (size_t)received;
    }
    return 0;
}

/* Once a framed UDP record has started on the byte stream, finish it before restoring the
 * socket's nonblocking mode; returning EAGAIN after a partial record would desynchronize framing. */
static int vm_udp_read_frame_bytes(struct pal_handle* handle, void* buf, size_t size) {
    bool was_nonblocking = handle->sock.is_nonblocking;
    handle->sock.is_nonblocking = false;
    int ret = vm_vsock_read_exact(handle, buf, size, /*force_nonblocking=*/false);
    handle->sock.is_nonblocking = was_nonblocking;
    return ret;
}

static int vm_udp_ensure_relay(struct pal_handle* handle) {
    if (!g_vsock)
        return -PAL_ERROR_AFNOSUPPORT;
    struct udp_rx_ring* ring = handle->region;
    if (ring->relay_connected)
        return 0;

    /* DNS creates non-blocking UDP sockets. Complete the control handshake synchronously before
     * honoring non-blocking behavior for framed datagram writes. */
    bool was_nonblocking = handle->sock.is_nonblocking;
    handle->sock.is_nonblocking = false;

    struct sockaddr_vm relay = {
        .svm_family = AF_VSOCK,
        .svm_cid = g_vsock->guest_cid,
        .svm_port = 65536 + g_vsock->guest_cid,
    };
    int ret = virtio_vsock_connect(handle->sock.fd, &relay, sizeof(relay),
                                   VSOCK_CONNECT_TIMEOUT_US);
    if (ret < 0) {
        handle->sock.is_nonblocking = was_nonblocking;
        return ret;
    }

    /* A UDP relay stream has no default peer; destinations travel with each datagram. */
    const uint8_t request[12] = { 'G', 'N', 'R', '1', 1, 1, 17, 0, 0, 0, 0, 0 };
    ret = vm_vsock_write_exact(handle, request, sizeof(request), /*force_nonblocking=*/false);
    uint8_t response[8];
    if (ret == 0)
        ret = vm_vsock_read_exact(handle, response, sizeof(response), /*force_nonblocking=*/false);
    if (ret < 0) {
        handle->sock.is_nonblocking = was_nonblocking;
        return ret;
    }
    if (memcmp(response, "GNR1", 4) || response[4] != 1 || response[5] != 0 ||
            response[6] || response[7]) {
        handle->sock.is_nonblocking = was_nonblocking;
        return -PAL_ERROR_CONNFAILED;
    }
    ring->relay_connected = true;
    handle->sock.is_nonblocking = was_nonblocking;
    log_debug("UDP VSOCK relay session established");
    return 0;
}

static inline uint32_t ring_used(uint32_t head, uint32_t tail, uint32_t cap) {
    return (tail >= head) ? (tail - head) : (cap - (head - tail));
}

static inline uint32_t ring_free(uint32_t head, uint32_t tail, uint32_t cap) {
    // reserve-1 convention
    return cap - ring_used(head, tail, cap) - 1;
}

static size_t sanitize_size(size_t size) {
    if (size > (1ull << 47)) {
        /* Some random approximation of what is a valid size. */
        return 0;
    }
    return size;
}

static size_t udp_pending_size(struct udp_rx_ring* ring) {
    if (!ring)
        return 0;

    uint8_t* data = ring_data(ring);
    uint32_t head = __atomic_load_n(&ring->head, __ATOMIC_RELAXED);
    uint32_t tail = __atomic_load_n(&ring->tail, __ATOMIC_ACQUIRE);
    if (head == tail)
        return 0;

    uint32_t len;
    memcpy(&len, data + head, sizeof(len));
    if (len == WRAP_MARKER) {
        if (tail == 0)
            return 0;
        memcpy(&len, data, sizeof(len));
    }

    return len <= ring->cap - sizeof(len) ? len : 0;
}

int pal_common_socket_wait_events(struct pal_handle* handle, pal_wait_flags_t events,
                                  pal_wait_flags_t* out_events) {
    pal_wait_flags_t revents = 0;

    /* The relay returns UDP responses through the VSOCK stream; poll it before checking readiness. */
    if (handle->sock.type == PAL_SOCKET_UDP && handle->region->relay_connected)
        (void)virtio_vsock_poll();
    if (handle->sock.type == PAL_SOCKET_TCP)
        vm_tcp_connect_progress(handle);

    spinlock_lock(&handle->sock.lock);

    if (handle->sock.type == PAL_SOCKET_UDP) {
        long peeked = handle->region->relay_connected ? virtio_vsock_peek(handle->sock.fd) : 0;
        if ((events & PAL_WAIT_READ) && (udp_pending_size(handle->region) || peeked > 0))
            revents |= PAL_WAIT_READ;
        if ((events & PAL_WAIT_WRITE) &&
                (!handle->region->relay_connected || virtio_vsock_can_write(handle->sock.fd)))
            revents |= PAL_WAIT_WRITE;
    } else {
        if (handle->sock.connect_pending)
            goto out;
        if (handle->sock.connect_finished && handle->sock.connect_result < 0) {
            revents = (events & (PAL_WAIT_READ | PAL_WAIT_WRITE)) | PAL_WAIT_ERROR;
            goto out;
        }

        long peeked = virtio_vsock_peek(handle->sock.fd);
        if (peeked < 0) {
            /* socket is invalid or was shutdown or in the process of closing */
            handle->flags |= PAL_HANDLE_FD_ERROR;
            revents = PAL_WAIT_ERROR;
            goto out;
        }

        if ((events & PAL_WAIT_READ) && peeked)
            revents |= PAL_WAIT_READ;
        if ((events & PAL_WAIT_WRITE) && virtio_vsock_can_write(handle->sock.fd))
            revents |= PAL_WAIT_WRITE;
    }

out:
    *out_events = revents;
    spinlock_unlock(&handle->sock.lock);
    return 0;
}

/* always returns the localhost address (127.0.0.1 for IPv4 and ::1 for IPv6); note that
 * pal_socket_addr::port is big-endian whereas sockaddr_vm::port is host-byte (little-endian) */
static void vm_to_pal_sockaddr(enum pal_socket_domain domain, const struct sockaddr_vm* vm_addr,
                               struct pal_socket_addr* pal_addr) {
    switch (domain) {
        case PAL_IPV4:;
            pal_addr->domain = PAL_IPV4;
            pal_addr->ipv4.port = htons((uint16_t)vm_addr->svm_port);

            uint8_t ipv4_localhost_addr[4] = {127, 0, 0, 1};
            memcpy(&pal_addr->ipv4.addr, ipv4_localhost_addr, sizeof(pal_addr->ipv4.addr));
            break;
        case PAL_IPV6:;
            pal_addr->domain = PAL_IPV6;
            pal_addr->ipv6.flowinfo = 0;
            pal_addr->ipv6.scope_id = 0;
            pal_addr->ipv6.port = htons((uint16_t)vm_addr->svm_port);

            uint8_t ipv6_localhost_addr[16] = {0, 0, 0, 0, 0, 0, 0, 0,
                                               0, 0, 0, 0, 0, 0, 0, 1};
            memcpy(pal_addr->ipv6.addr, ipv6_localhost_addr, sizeof(pal_addr->ipv6.addr));
            break;
        case PAL_DISCONNECT:
            pal_addr->domain = PAL_DISCONNECT;
            break;
        default:
            BUG();
    }
}

static void pal_to_vm_sockaddr(const struct pal_socket_addr* pal_addr, struct sockaddr_vm* vm_addr) {
    switch (pal_addr->domain) {
        case PAL_IPV4:;
            vm_addr->svm_family = AF_VSOCK;
            vm_addr->svm_port   = ntohs(pal_addr->ipv4.port);
            break;
        case PAL_IPV6:;
            vm_addr->svm_family = AF_VSOCK;
            vm_addr->svm_port   = ntohs(pal_addr->ipv6.port);
            break;
        case PAL_DISCONNECT:
            log_error("connect(AF_UNSPEC) is not yet implemented!");
            BUG();
        default:
            BUG();
    }
}

static struct pal_handle* create_sock_handle(int fd, enum pal_socket_domain domain,
                                             enum pal_socket_type type,
                                             struct handle_ops* handle_ops, struct socket_ops* ops,
                                             bool is_nonblocking) {
    struct pal_handle* handle = calloc(1, sizeof(*handle));
    if (!handle)
        return NULL;

    handle->hdr.type = PAL_TYPE_SOCKET;
    handle->hdr.ops = handle_ops;
    handle->flags |= PAL_HANDLE_FD_READABLE | PAL_HANDLE_FD_WRITABLE;

    spinlock_init(&handle->sock.lock);
    handle->sock.fd = fd;
    handle->sock.domain = domain;
    handle->sock.type = type;
    handle->sock.ops = ops;
    handle->sock.recv_buf_size = g_default_recv_buf_size;
    handle->sock.send_buf_size = g_default_send_buf_size;
    handle->sock.linger = 0;
    handle->sock.recvtimeout_us = 0;
    handle->sock.sendtimeout_us = 0;
    handle->sock.is_nonblocking = is_nonblocking;
    handle->sock.reuseaddr = false;
    handle->sock.reuseport = false;
    handle->sock.keepalive = false;
    handle->sock.broadcast = false;
    handle->sock.tcp_cork = false;
    handle->sock.tcp_keepidle = DEFAULT_TCP_KEEPIDLE;
    handle->sock.tcp_keepintvl = DEFAULT_TCP_KEEPINTVL;
    handle->sock.tcp_keepcnt = DEFAULT_TCP_KEEPCNT;
    handle->sock.tcp_user_timeout = DEFAULT_TCP_USER_TIMEOUT;
    handle->sock.tcp_nodelay = false;
    handle->sock.ipv6_v6only = false;

    return handle;
}

int pal_common_socket_create(enum pal_socket_domain domain, enum pal_socket_type type,
                             pal_stream_options_t options, struct pal_handle** out_handle) {
    assert(domain == PAL_IPV4 || domain == PAL_IPV6);

    /* no vsock device: TCP/UDP sockets are emulated over vsock, so fail instead of crashing */
    if (!g_vsock)
        return -PAL_ERROR_AFNOSUPPORT;

    /* Testing for VSOCK */
    log_debug("socket: domain=%d, type=%d, options=0x%x", domain, type, options);

    struct handle_ops* handle_ops = NULL;
    struct socket_ops* sock_ops = NULL;
    switch (type) {
        case PAL_SOCKET_TCP:
            handle_ops = &g_tcp_handle_ops;
            sock_ops = &g_tcp_sock_ops;
            break;
        case PAL_SOCKET_UDP:
            handle_ops = &g_udp_handle_ops;
            sock_ops = &g_udp_sock_ops;
            break;
        default:
            log_always("ERROR: unknown connection type.. (BUG)\n");
            BUG();
    }

    int fd = virtio_vsock_socket(AF_VSOCK, VIRTIO_VSOCK_TYPE_STREAM, /*protocol=*/0);
    if (fd < 0)
        return fd;

    struct pal_handle* handle = create_sock_handle(fd, domain, type, handle_ops, sock_ops,
                                                   !!(options & PAL_OPTION_NONBLOCK));
    if (!handle) {
        int ret = virtio_vsock_close(fd, VSOCK_CLOSE_TIMEOUT_US);
        if (ret < 0) {
            log_error("closing socket fd failed: %s", pal_strerror(ret));
        }
        return -PAL_ERROR_NOMEM;
    }

    /* Debug log */
    log_debug("socket: created with fd %d for SOCK-TYPE: %d\n", fd, type);

    *out_handle = handle;
    return 0;
}

static void pal_common_socket_destroy(struct pal_handle* handle) {
    assert(handle->hdr.type == PAL_TYPE_SOCKET);

    if (handle->region) {
        spinlock_lock(&g_sock_region_map_lock);
        int dst = 0;
        for (int src = 0; src < current_indx_in_sock_region_map; src++) {
            if (SOCK_REGION_MAP[src].region_handle != handle) {
                SOCK_REGION_MAP[dst++] = SOCK_REGION_MAP[src];
            }
        }
        current_indx_in_sock_region_map = dst;
        spinlock_unlock(&g_sock_region_map_lock);
    }

    spinlock_lock(&handle->sock.lock);
    int ret = virtio_vsock_close(handle->sock.fd, VSOCK_CLOSE_TIMEOUT_US);
    spinlock_unlock(&handle->sock.lock);

    if (ret < 0) {
        log_error("closing socket fd failed: %s", pal_strerror(ret));
        /* We cannot do anything about it anyway... */
    }

    free(handle);
}

static int pal_common_socket_bind(struct pal_handle* handle, struct pal_socket_addr* addr) {
    /* Log for bind requests. */
    log_debug("[graphics] Bind request arrived.\n");
    
    assert(handle->hdr.type == PAL_TYPE_SOCKET);
    if (!g_vsock)
        return -PAL_ERROR_AFNOSUPPORT;
    if (addr->domain != handle->sock.domain) {
        return -PAL_ERROR_INVAL;
    }

    struct sockaddr_vm addr_vm = { .svm_cid = g_vsock->guest_cid };
    pal_to_vm_sockaddr(addr, &addr_vm);
    uint16_t existing_port = fetch_guest_port_given_sockfd(handle->sock.fd);

    uint16_t new_port = 0;
    int ret = virtio_vsock_bind(handle->sock.fd, &addr_vm, sizeof(addr_vm), &new_port,
                                handle->sock.domain == PAL_IPV4, handle->sock.ipv6_v6only,
                                handle->sock.reuseport);
    if (ret < 0)
        return ret;

    switch (addr->domain) {
        case PAL_IPV4:
            if (!addr->ipv4.port) {
                addr->ipv4.port = htons(new_port);
            }
            break;
        case PAL_IPV6:
            if (!addr->ipv6.port) {
                addr->ipv6.port = htons(new_port);
            }
            break;
        default:
            BUG();
    }
    if (handle->region){
        spinlock_lock(&g_sock_region_map_lock);
        for(int idx=0; idx<current_indx_in_sock_region_map; idx++){
            if (SOCK_REGION_MAP[idx].region_handle == handle){
                SOCK_REGION_MAP[idx].port_num = new_port;
                log_debug("Rewiring port to %d from %d\n", new_port, handle->sock.fd);
                break;
            }
        }
        spinlock_unlock(&g_sock_region_map_lock);
    }
    log_debug("[graphics] Bind request DONE.\n");
    return 0;
}

static int pal_common_tcp_listen(struct pal_handle* handle, unsigned int backlog) {
    assert(handle->hdr.type == PAL_TYPE_SOCKET);
    return virtio_vsock_listen(handle->sock.fd, backlog);
}

static int pal_common_tcp_accept(struct pal_handle* handle, pal_stream_options_t options,
                          struct pal_handle** out_client, struct pal_socket_addr* out_client_addr,
                          struct pal_socket_addr* out_local_addr) {
    int ret;
    assert(handle->hdr.type == PAL_TYPE_SOCKET);

    spinlock_lock(&handle->sock.lock);

    int client_fd;
    struct sockaddr_vm client_addr_vm = {0};
    size_t client_addr_vm_size = sizeof(client_addr_vm);

    while (true) {
        client_fd = virtio_vsock_accept(handle->sock.fd, &client_addr_vm, &client_addr_vm_size);
        if (client_fd < 0) {
            if (client_fd == -PAL_ERROR_TRYAGAIN && !handle->sock.is_nonblocking) {
                int wret = sched_thread_wait_interruptible(&g_sockets_reader_futex,
                                                           &handle->sock.lock);
                if (wret < 0) {
                    spinlock_unlock(&handle->sock.lock);
                    return wret;
                }
                continue;
            }
            spinlock_unlock(&handle->sock.lock);
            return client_fd;
        }

        /* accept succeeded */
        break;
    }

    spinlock_unlock(&handle->sock.lock); /* done with listening socket here */

    struct pal_handle* client = create_sock_handle(client_fd, handle->sock.domain,
                                                   handle->sock.type, handle->hdr.ops,
                                                   handle->sock.ops,
                                                   !!(options & PAL_OPTION_NONBLOCK));
    if (!client) {
        ret = virtio_vsock_close(client_fd, VSOCK_CLOSE_TIMEOUT_US);
        if (ret < 0) {
            log_error("closing socket fd failed: %s", pal_strerror(ret));
        }
        return -PAL_ERROR_NOMEM;
    }

    struct sockaddr_vm local_addr_vm = {0};
    size_t local_addr_vm_size = sizeof(local_addr_vm);
    ret = virtio_vsock_getsockname(client_fd, &local_addr_vm, &local_addr_vm_size);
    if (ret < 0) {
        _PalObjectDestroy(client);
        return ret;
    }

    if (out_client_addr) {
        vm_to_pal_sockaddr(client->sock.domain, &client_addr_vm, out_client_addr);
    }
    if (out_local_addr) {
        vm_to_pal_sockaddr(client->sock.domain, &local_addr_vm, out_local_addr);
    }

    *out_client = client;
    return 0;
}

static int pal_common_socket_connect(struct pal_handle* handle, struct pal_socket_addr* addr,
                                     struct pal_socket_addr* out_local_addr,
                                     bool* out_inprogress) {
    assert(handle->hdr.type == PAL_TYPE_SOCKET);
    if (!g_vsock)
        return -PAL_ERROR_AFNOSUPPORT;
    if (addr->domain != PAL_DISCONNECT && addr->domain != handle->sock.domain) {
        return -PAL_ERROR_INVAL;
    }

    /* UDP is emulated by the in-guest datagram rings below, not by connecting the backing VSOCK
     * stream. Linux UDP connect merely records a default peer, which LibOS already tracks. */
    if (handle->sock.type == PAL_SOCKET_UDP) {
        if (out_local_addr) {
            struct sockaddr_vm local_addr_vm = {
                .svm_port = fetch_guest_port_given_sockfd(handle->sock.fd),
            };
            vm_to_pal_sockaddr(handle->sock.domain, &local_addr_vm, out_local_addr);
        }
        *out_inprogress = false;
        return 0;
    }

    if (addr->domain == PAL_DISCONNECT)
        return -PAL_ERROR_NOTSUPPORT;

    uint16_t remote_port = addr->domain == PAL_IPV4 ? ntohs(addr->ipv4.port)
                                                    : ntohs(addr->ipv6.port);
    bool direct_vsock_port = vm_is_direct_vsock_port(remote_port);
    struct sockaddr_vm addr_vm = {
        .svm_cid = direct_vsock_port ? g_vsock->host_cid : g_vsock->guest_cid,
        .svm_port = direct_vsock_port ? 0 : 65536 + g_vsock->guest_cid,
    };
    pal_to_vm_sockaddr(addr, &addr_vm);
    if (!direct_vsock_port) {
        /* pal_to_vm_sockaddr() copies only the app's port; outbound TCP uses the reserved
         * per-guest relay port and carries its numeric destination in a handshake below. */
        addr_vm.svm_port = 65536 + g_vsock->guest_cid;
    }

    int ret;
    if (handle->sock.is_nonblocking) {
        spinlock_lock(&handle->sock.lock);
        if (!direct_vsock_port) {
            ret = vm_prepare_relay_connect(handle, addr);
            if (ret < 0) {
                spinlock_unlock(&handle->sock.lock);
                return ret;
            }
        }

        handle->sock.connect_pending = true;
        handle->sock.connect_finished = false;
        handle->sock.connect_direct_vsock = direct_vsock_port;
        handle->sock.connect_result = 0;
        ret = virtio_vsock_connect_start(handle->sock.fd, &addr_vm, sizeof(addr_vm));
        if (ret < 0) {
            handle->sock.connect_pending = false;
            spinlock_unlock(&handle->sock.lock);
            return ret;
        }

        struct sockaddr_vm local_addr_vm = {0};
        size_t local_addr_vm_size = sizeof(local_addr_vm);
        ret = virtio_vsock_getsockname(handle->sock.fd, &local_addr_vm, &local_addr_vm_size);
        if (ret < 0) {
            handle->sock.connect_pending = false;
            spinlock_unlock(&handle->sock.lock);
            return ret;
        }
        if (out_local_addr)
            vm_to_pal_sockaddr(handle->sock.domain, &local_addr_vm, out_local_addr);
        *out_inprogress = true;
        spinlock_unlock(&handle->sock.lock);
        return 0;
    }

    ret = virtio_vsock_connect(handle->sock.fd, &addr_vm, sizeof(addr_vm),
                               VSOCK_CONNECT_TIMEOUT_US);
    if (ret < 0)
        return ret;

    if (!direct_vsock_port) {
        ret = vm_relay_connect(handle, addr);
        if (ret < 0)
            return ret;
    }

    struct sockaddr_vm local_addr_vm = {0};
    size_t local_addr_vm_size = sizeof(local_addr_vm);
    ret = virtio_vsock_getsockname(handle->sock.fd, &local_addr_vm, &local_addr_vm_size);
    if (ret < 0)
        return ret;

    if (out_local_addr) {
        vm_to_pal_sockaddr(handle->sock.domain, &local_addr_vm, out_local_addr);
    }

    *out_inprogress = false; /* VM PALs do not currently emulate EINPROGRESS */
    return 0;
}

static int pal_common_socket_attrquerybyhdl(struct pal_handle* handle, PAL_STREAM_ATTR* attr) {
    assert(handle->hdr.type == PAL_TYPE_SOCKET);

    spinlock_lock(&handle->sock.lock);

    memset(attr, 0, sizeof(*attr));

    attr->handle_type = PAL_TYPE_SOCKET;
    attr->nonblocking = handle->sock.is_nonblocking;

    if (handle->sock.type == PAL_SOCKET_UDP) {
        long peeked = handle->region->relay_connected ? virtio_vsock_peek(handle->sock.fd) : 0;
        attr->pending_size = udp_pending_size(handle->region) ? udp_pending_size(handle->region)
                                                              : peeked > 0 ? sanitize_size(peeked)
                                                                           : 0;
    } else {
        long peeked = virtio_vsock_peek(handle->sock.fd);
        attr->pending_size = peeked >= 0 ? sanitize_size(peeked) : 0;
    }

    attr->socket.linger = handle->sock.linger;
    attr->socket.recv_buf_size = handle->sock.recv_buf_size;
    attr->socket.send_buf_size = handle->sock.send_buf_size;
    attr->socket.receivetimeout_us = handle->sock.recvtimeout_us;
    attr->socket.sendtimeout_us = handle->sock.sendtimeout_us;
    attr->socket.reuseaddr = handle->sock.reuseaddr;
    attr->socket.reuseport = handle->sock.reuseport;
    attr->socket.keepalive = handle->sock.keepalive;
    attr->socket.broadcast = handle->sock.broadcast;
    attr->socket.tcp_cork = handle->sock.tcp_cork;
    attr->socket.tcp_keepidle = handle->sock.tcp_keepidle;
    attr->socket.tcp_keepintvl = handle->sock.tcp_keepintvl;
    attr->socket.tcp_keepcnt = handle->sock.tcp_keepcnt;
    attr->socket.tcp_nodelay = handle->sock.tcp_nodelay;
    attr->socket.tcp_user_timeout = handle->sock.tcp_user_timeout;
    attr->socket.ipv6_v6only = handle->sock.ipv6_v6only;

    spinlock_unlock(&handle->sock.lock);
    return 0;
};

static int pal_common_socket_attrsetbyhdl(struct pal_handle* handle, PAL_STREAM_ATTR* attr) {
    spinlock_lock(&handle->sock.lock);
    handle->sock.is_nonblocking = attr->nonblocking;
    handle->sock.ipv6_v6only    = attr->socket.ipv6_v6only;
    handle->sock.reuseport      = attr->socket.reuseport;
    /* SO_RCVTIMEO / SO_SNDTIMEO: the LibOS sets them here; recv uses the receive timeout (see
     * vm_sock_recv_wait()), the send timeout is only stored and reported back */
    handle->sock.recvtimeout_us = attr->socket.receivetimeout_us;
    handle->sock.sendtimeout_us = attr->socket.sendtimeout_us;

    int ret = virtio_vsock_set_socket_options(handle->sock.fd, handle->sock.ipv6_v6only,
                                              handle->sock.reuseport);

    spinlock_unlock(&handle->sock.lock);
    return ret;
}

static int pal_common_tcp_send(struct pal_handle* handle, struct iovec* iov, size_t iov_len,
                               size_t* out_size, struct pal_socket_addr* addr,
                               bool force_nonblocking) {
    assert(handle->hdr.type == PAL_TYPE_SOCKET);
        // log_always("[pal_common_tcp_send] handle=%p; iov=%p, iov_len=%d, force_nonblocking=%d\n", handle, iov, iov_len, force_nonblocking);

    spinlock_lock(&handle->sock.lock);

    size_t total_bytes = 0;
    size_t iov_idx = 0;
    while (iov_idx < iov_len) {
        if (!iov[iov_idx].iov_base || !iov[iov_idx].iov_len) {
            iov_idx++;
            continue;
        }

        int64_t bytes = virtio_vsock_write(handle->sock.fd, iov[iov_idx].iov_base,
                                           iov[iov_idx].iov_len);
        if (bytes < 0) {
            if (bytes != -PAL_ERROR_TRYAGAIN) {
                /* unrecoverable error, fail immediately */
                spinlock_unlock(&handle->sock.lock);
                return bytes;
            }
            if (total_bytes) {
                /* don't wait/error out if sent something; consider this call successful */
                goto out;
            }
            if (!handle->sock.is_nonblocking && !force_nonblocking) {
                /* blocking socket that didn't send anything must wait */
                int wret = sched_thread_wait_interruptible(&g_sockets_writer_futex,
                                                           &handle->sock.lock);
                if (wret < 0) {
                    spinlock_unlock(&handle->sock.lock);
                    return wret;
                }
                continue;
            }
            /* non-blocking socket that didn't send anything must error out with TRYAGAIN */
            spinlock_unlock(&handle->sock.lock);
            return -PAL_ERROR_TRYAGAIN;
        }

        /* write succeeded, at least partially */
        total_bytes += bytes;

        if ((size_t)bytes < iov[iov_idx].iov_len) {
            /* partial write, let's not try further; should be a rare condition */
            goto out;
        }

        assert((size_t)bytes == iov[iov_idx].iov_len);
        iov_idx++;
    }

out:
    spinlock_unlock(&handle->sock.lock);
    *out_size = total_bytes;
    return 0;
}

static int pal_common_udp_send(struct pal_handle *handle,
                               struct iovec *iov, size_t iov_len,
                               size_t *out_size,
                               struct pal_socket_addr *addr,
                               bool force_nonblocking)
{
    __UNUSED(force_nonblocking);
    if (!g_vsock)
        return -PAL_ERROR_AFNOSUPPORT;
    // if(iov[0].iov_len > 100)
    //     log_always("[pal_common_udp_send] handle=%p; iov=%p, iov_len=%d, force_nonblocking=%d\n", handle, iov, iov_len, force_nonblocking);
    assert(handle->hdr.type == PAL_TYPE_SOCKET);
    struct udp_rx_ring *r = NULL;
    struct sockaddr_vm addr_vm = { .svm_cid = g_vsock->host_cid };
    pal_to_vm_sockaddr(addr, &addr_vm);

    spinlock_lock(&g_sock_region_map_lock);
    for(int idx=0; idx<current_indx_in_sock_region_map; idx++){
        // log_always("Port to send: %d\n; Current: %d\n", addr_vm.svm_port, SOCK_REGION_MAP[idx].port_num); 
        if (SOCK_REGION_MAP[idx].port_num == addr_vm.svm_port){
            // log_always("[graphics] Sending the data to port_num: %d from %d", addr_vm.svm_port, fetch_guest_port_given_sockfd(handle->sock.fd));
            r = (struct udp_rx_ring *)(SOCK_REGION_MAP[idx].region_handle->region);
            // log_always("R = %p\n", r);
            break;
        }
    }
    if (!r) {
        spinlock_unlock(&g_sock_region_map_lock);
        if (!addr || (addr->domain != PAL_IPV4 && addr->domain != PAL_IPV6))
            return -PAL_ERROR_AFNOSUPPORT;

        size_t payload_size = 0;
        for (size_t i = 0; i < iov_len; i++) {
            if (iov[i].iov_len > SIZE_MAX - payload_size)
                return -PAL_ERROR_TOOLONG;
            payload_size += iov[i].iov_len;
        }
        if (payload_size > 65507)
            return -PAL_ERROR_TOOLONG;

        uint8_t family;
        const uint8_t* destination;
        uint8_t address_size;
        uint16_t port;
        if (addr->domain == PAL_IPV4) {
            family = 4;
            destination = (const uint8_t*)&addr->ipv4.addr;
            address_size = 4;
            port = ntohs(addr->ipv4.port);
        } else {
            family = 6;
            destination = addr->ipv6.addr;
            address_size = 16;
            port = ntohs(addr->ipv6.port);
        }
        if (!port)
            return -PAL_ERROR_INVAL;

        log_debug("UDP relay send to %u.%u.%u.%u:%u, %zu bytes", destination[0],
                  address_size > 1 ? destination[1] : 0,
                  address_size > 2 ? destination[2] : 0,
                  address_size > 3 ? destination[3] : 0, port, payload_size);

        spinlock_lock(&handle->sock.lock);
        int ret = vm_udp_ensure_relay(handle);
        if (ret < 0) {
            spinlock_unlock(&handle->sock.lock);
            return ret;
        }
        uint8_t header[6] = {
            family, address_size, port >> 8, port & 0xff,
            payload_size >> 8, payload_size & 0xff,
        };
        /* A partial frame cannot be retried safely as a new datagram on this stream. */
        bool was_nonblocking = handle->sock.is_nonblocking;
        handle->sock.is_nonblocking = false;
        ret = vm_vsock_write_exact(handle, header, sizeof(header), /*force_nonblocking=*/false);
        if (ret == 0)
            ret = vm_vsock_write_exact(handle, destination, address_size,
                                       /*force_nonblocking=*/false);
        for (size_t i = 0; ret == 0 && i < iov_len; i++) {
            if (iov[i].iov_base && iov[i].iov_len)
                ret = vm_vsock_write_exact(handle, iov[i].iov_base, iov[i].iov_len,
                                           /*force_nonblocking=*/false);
        }
        handle->sock.is_nonblocking = was_nonblocking;
        spinlock_unlock(&handle->sock.lock);
        if (ret < 0)
            return ret;
        *out_size = payload_size;
        return 0;
    }
    spinlock_unlock(&g_sock_region_map_lock);

    uint8_t *data = ring_data(r);
    uint32_t cap = r->cap;

    /* One datagram per send, whatever the number of iovecs (a record per iovec split a gathered
     * sendmsg()/sendmmsg() into several datagrams; LTP sendmmsg01). */
    size_t total = 0;
    for (size_t i = 0; i < iov_len; i++) {
        if (iov[i].iov_base && iov[i].iov_len) {
            if (iov[i].iov_len > 65507 - total)
                return -PAL_ERROR_TOOLONG;
            total += iov[i].iov_len;
        }
    }
    uint32_t len  = (uint32_t)total;
    uint32_t need = sizeof(uint32_t) + len;

    spinlock_lock(&handle->sock.lock);

    uint32_t head = __atomic_load_n(&r->head, __ATOMIC_ACQUIRE);
    uint32_t tail = __atomic_load_n(&r->tail, __ATOMIC_RELAXED);

    /* total space check; UDP semantics: a datagram that does not fit is silently dropped */
    if (ring_free(head, tail, cap) >= need) {
        /* contiguous space check */
        if (tail + need > cap) {
            /* emit wrap marker if it fits */
            if (cap - tail >= sizeof(uint32_t)) {
                uint32_t marker = WRAP_MARKER;
                memcpy(data + tail, &marker, sizeof(marker));
            }
            tail = 0;
        }

        /* write record: [len][payload] */
        memcpy(data + tail, &len, sizeof(uint32_t));
        size_t off = tail + sizeof(uint32_t);
        for (size_t i = 0; i < iov_len; i++) {
            if (!iov[i].iov_base || !iov[i].iov_len)
                continue;
            memcpy(data + off, iov[i].iov_base, iov[i].iov_len);
            off += iov[i].iov_len;
        }

        /* publish tail last (release) */
        __atomic_store_n(&r->tail, tail + need, __ATOMIC_RELEASE);
        __atomic_store_n(&r->sender_port, fetch_guest_port_given_sockfd(handle->sock.fd),
                         __ATOMIC_RELEASE);

        sched_thread_wakeup(&r->udp_rx_futex);
        sched_thread_wakeup(&g_streams_waiting_events_futex);
    }

    spinlock_unlock(&handle->sock.lock);
    *out_size = total;

    return 0;
}


/* Sleep on `futex` of a blocking receive (the caller holds handle->sock.lock) until it is woken or
 * the socket's SO_RCVTIMEO has run out. `*deadline_us` is the absolute deadline, 0 before the first
 * wait; it is set here and must be kept across the caller's wait loop. Returns 0 after a wakeup (the
 * caller re-checks its condition) and -PAL_ERROR_TRYAGAIN once the timeout is over. Without a timeout
 * this is plain sched_thread_wait(); before this, a receive timeout was ignored and a blocking
 * recv() on an idle socket never returned. */
static int vm_sock_recv_wait(struct pal_handle* handle, int* futex, uint64_t* deadline_us) {
    uint64_t timeout_us = handle->sock.recvtimeout_us;
    if (!timeout_us)
        return sched_thread_wait_interruptible(futex, &handle->sock.lock);

    uint64_t now_us;
    int ret = get_time_in_us(&now_us);
    if (ret < 0)
        return ret;
    if (!*deadline_us)
        *deadline_us = now_us + timeout_us;
    if (now_us >= *deadline_us)
        return -PAL_ERROR_TRYAGAIN;

    void* timeout = NULL;
    ret = register_timeout(*deadline_us, futex, &timeout);
    if (ret < 0)
        return ret;
    ret = sched_thread_wait_interruptible(futex, &handle->sock.lock);
    deregister_timeout(timeout);
    return ret;
}

static int pal_common_tcp_recv(struct pal_handle* handle, struct iovec* iov, size_t iov_len,
                               size_t* out_total_size, struct pal_socket_addr* addr,
                               bool force_nonblocking) {
    __UNUSED(addr);
    assert(handle->hdr.type == PAL_TYPE_SOCKET);

    /* recv() may be used without a preceding readiness query. Poll the shared rings here as well
     * so a nonblocking consumer observes newly arrived data. */
    (void)virtio_vsock_poll();

    spinlock_lock(&handle->sock.lock);

    size_t total_bytes = 0;
    size_t iov_idx = 0;
    uint64_t deadline_us = 0;
    while (iov_idx < iov_len) {
        if (!iov[iov_idx].iov_base || !iov[iov_idx].iov_len) {
            iov_idx++;
            continue;
        }

        int64_t bytes = virtio_vsock_read(handle->sock.fd, iov[iov_idx].iov_base,
                                          iov[iov_idx].iov_len);
        if (bytes < 0) {
            if (bytes != -PAL_ERROR_TRYAGAIN) {
                /* unrecoverable error, fail immediately */
                spinlock_unlock(&handle->sock.lock);
                return bytes;
            }
            if (total_bytes) {
                /* don't wait/error out if received something; consider this call successful */
                goto out;
            }
            if (!handle->sock.is_nonblocking && !force_nonblocking) {
                /* blocking socket that didn't receive anything must wait */
                spinlock_unlock(&handle->sock.lock);
                bool received = virtio_vsock_busy_poll(/*timeout_us=*/25);
                spinlock_lock(&handle->sock.lock);
                if (received)
                    continue;
                if (virtio_vsock_rearm_notifications()) {
                    spinlock_unlock(&handle->sock.lock);
                    (void)virtio_vsock_poll();
                    spinlock_lock(&handle->sock.lock);
                    continue;
                }
                int wait_ret = vm_sock_recv_wait(handle, &g_sockets_reader_futex, &deadline_us);
                if (wait_ret < 0) {
                    spinlock_unlock(&handle->sock.lock);
                    return wait_ret;
                }
                continue;
            }

            /* non-blocking socket that didn't receive anything must error out with TRYAGAIN */
            spinlock_unlock(&handle->sock.lock);
            return -PAL_ERROR_TRYAGAIN;
        }

        /* read succeeded, at least partially */
        total_bytes += bytes;

        if ((size_t)bytes < iov[iov_idx].iov_len) {
            /* partial read, let's not try further; should be a rare condition */
            goto out;
        }

        assert((size_t)bytes == iov[iov_idx].iov_len);
        iov_idx++;
    }

out:
    spinlock_unlock(&handle->sock.lock);
    *out_total_size = total_bytes;
    return 0;
}

/* `int`, not ssize_t: struct socket_ops.recv is declared `int` (tcp twin above)
 * and every return here is an error code or a small byte count. gcc >= 14 made
 * the ops-table pointer-type mismatch a hard error. */
static int pal_common_udp_recv(struct pal_handle *handle,
                               struct iovec *iov, size_t iov_len,
                               size_t *out_total_size,
                               struct pal_socket_addr *addr,
                               bool force_nonblocking)
{
    __UNUSED(addr);
    assert(handle->hdr.type == PAL_TYPE_SOCKET);
    // if(iov[0].iov_len > 100)
        // log_always("[pal_common_udp_recv] handle=%p; iov[len=%d]=%p, iov_len=%d, force_nonblocking=%d\n", handle, iov[0].iov_len, iov_len, force_nonblocking);
    // force_nonblocking = 1;
    struct udp_rx_ring *r = (struct udp_rx_ring *)handle->region;
    // log_always("R = %p; My port is: %d\n", r, fetch_guest_port_given_sockfd(handle->sock.fd));
    assert(r!=NULL);
    // for(int idx=0; idx<current_indx_in_sock_region_map; idx++){
    //     if (SOCK_REGION_MAP[idx].port_num == fetch_guest_port_given_sockfd(handle->sock.fd)){
    //         log_always("R=%p; New=%p; are they same? %d\n", r, (struct udp_rx_ring *)(SOCK_REGION_MAP[idx].region_handle->region), r == (struct udp_rx_ring *)(SOCK_REGION_MAP[idx].region_handle->region));
    //         assert(r == (struct udp_rx_ring *)(SOCK_REGION_MAP[idx].region_handle->region));
    //         break;
    //     }
    // }
    uint8_t *data = ring_data(r);
    uint32_t cap = r->cap;

    if (!iov_len || !iov[0].iov_base) {
        *out_total_size = 0;
        return 0;
    }

    /* Network responses are queued in the VSOCK driver before they enter this UDP ring. */
    if (r->relay_connected)
        (void)virtio_vsock_poll();

    uint64_t deadline_us = 0;
    spinlock_lock(&handle->sock.lock);
    for (;;) {
        uint32_t head = __atomic_load_n(&r->head, __ATOMIC_RELAXED);
        uint32_t tail = __atomic_load_n(&r->tail, __ATOMIC_ACQUIRE);
        uint32_t sender_port = __atomic_load_n(&r->sender_port, __ATOMIC_ACQUIRE);

        if (head == tail) {
            if (r->relay_connected && virtio_vsock_peek(handle->sock.fd) > 0) {
                uint8_t header[6];
                int ret = vm_udp_read_frame_bytes(handle, header, sizeof(header));
                if (ret < 0) {
                    spinlock_unlock(&handle->sock.lock);
                    return ret;
                }
                uint8_t family = header[0];
                uint8_t address_size = header[1];
                uint16_t source_port = ((uint16_t)header[2] << 8) | header[3];
                uint16_t payload_size = ((uint16_t)header[4] << 8) | header[5];
                if (family == 0 && address_size == 1 && !payload_size) {
                    spinlock_unlock(&handle->sock.lock);
                    return -unix_to_pal_error(source_port);
                }
                if (!((family == 4 && address_size == 4) ||
                      (family == 6 && address_size == 16)) || !source_port) {
                    spinlock_unlock(&handle->sock.lock);
                    return -PAL_ERROR_CONNFAILED;
                }
                uint8_t source[16];
                ret = vm_udp_read_frame_bytes(handle, source, address_size);
                uint8_t* payload = malloc(payload_size ? payload_size : 1);
                if (!payload) {
                    spinlock_unlock(&handle->sock.lock);
                    return -PAL_ERROR_NOMEM;
                }
                if (ret == 0)
                    ret = vm_udp_read_frame_bytes(handle, payload, payload_size);
                if (ret < 0) {
                    free(payload);
                    spinlock_unlock(&handle->sock.lock);
                    return ret;
                }

                size_t copied = 0;
                for (size_t i = 0; i < iov_len && copied < payload_size; i++) {
                    size_t chunk = MIN(payload_size - copied, iov[i].iov_len);
                    if (iov[i].iov_base && chunk)
                        memcpy(iov[i].iov_base, payload + copied, chunk);
                    copied += chunk;
                }
                free(payload);
                if (addr) {
                    if (family == 4) {
                        addr->domain = PAL_IPV4;
                        addr->ipv4.port = htons(source_port);
                        memcpy(&addr->ipv4.addr, source, 4);
                    } else {
                        addr->domain = PAL_IPV6;
                        addr->ipv6.port = htons(source_port);
                        memcpy(addr->ipv6.addr, source, 16);
                    }
                }
                spinlock_unlock(&handle->sock.lock);
                *out_total_size = copied;
                return 0;
            }

            if (!handle->sock.is_nonblocking && !force_nonblocking) {
                int wait_ret = vm_sock_recv_wait(handle, r->relay_connected ? &g_sockets_reader_futex
                                                                            : &r->udp_rx_futex,
                                                 &deadline_us);
                if (wait_ret < 0) {
                    spinlock_unlock(&handle->sock.lock);
                    return wait_ret;
                }
                continue;
            }
            spinlock_unlock(&handle->sock.lock);
            return -PAL_ERROR_TRYAGAIN;
        }

        uint32_t len;
        memcpy(&len, data + head, sizeof(uint32_t));

        /* wrap marker */
        if (len == WRAP_MARKER) {
            __atomic_store_n(&r->head, 0, __ATOMIC_RELEASE);
            continue;   // retry under same lock
        }

        /* scatter the datagram over the iovecs; what does not fit is dropped, as for UDP */
        size_t copy = 0;
        for (size_t i = 0; i < iov_len && copy < len; i++) {
            if (!iov[i].iov_base || !iov[i].iov_len)
                continue;
            size_t n = MIN(iov[i].iov_len, (size_t)len - copy);
            memcpy(iov[i].iov_base, data + head + sizeof(uint32_t) + copy, n);
            copy += n;
        }
        // log_always("My port is: %d\n", fetch_guest_port_given_sockfd(handle->sock.fd));
        // log_always("Read size = %d at %p and  %s  - from the shmem at %p - size=%d\n", len, data + head, data + head + sizeof(uint32_t), data + head + sizeof(uint32_t), copy);

        __atomic_store_n(&r->head,
            head + sizeof(uint32_t) + len,
            __ATOMIC_RELEASE);
        if (addr) {
            addr->domain = PAL_IPV4;
            addr->ipv4.port = htons(sender_port);

            uint8_t ipv4_localhost_addr[4] = {127, 0, 0, 1};
            memcpy(&addr->ipv4.addr, ipv4_localhost_addr, sizeof(addr->ipv4.addr));
        }
        spinlock_unlock(&handle->sock.lock);
        *out_total_size = copy;
        return 0;
    }
}


static int pal_common_tcp_delete(struct pal_handle* handle, enum pal_delete_mode mode) {
    assert(handle->hdr.type == PAL_TYPE_SOCKET);

    spinlock_lock(&handle->sock.lock);

    enum virtio_vsock_shutdown shutdown;
    switch (mode) {
        case PAL_DELETE_ALL:
            shutdown = VIRTIO_VSOCK_SHUTDOWN_COMPLETE;
            break;
        case PAL_DELETE_READ:
            shutdown = VIRTIO_VSOCK_SHUTDOWN_RCV;
            break;
        case PAL_DELETE_WRITE:
            shutdown = VIRTIO_VSOCK_SHUTDOWN_SEND;
            break;
        default:
            spinlock_unlock(&handle->sock.lock);
            return -PAL_ERROR_INVAL;
    }

    int ret = virtio_vsock_shutdown(handle->sock.fd, shutdown);

    spinlock_unlock(&handle->sock.lock);
    return ret;
}

static int pal_common_udp_delete(PAL_HANDLE handle, enum pal_delete_mode mode) {
    __UNUSED(handle);
    __UNUSED(mode);

    log_always("pal_common_udp_delete: UDP delete is not implemented.");
    
    return 0;
}

static int udp_register_shmem(struct pal_handle* handle, void *region){
    spinlock_lock(&g_sock_region_map_lock);
    if ((size_t)current_indx_in_sock_region_map >= ARRAY_SIZE(SOCK_REGION_MAP)) {
        spinlock_unlock(&g_sock_region_map_lock);
        log_error("UDP socket-region map is full");
        return -PAL_ERROR_NOMEM;
    }
    udp_rx_ring_init(handle, region);
    SOCK_REGION_MAP[current_indx_in_sock_region_map].region_handle = handle;
    SOCK_REGION_MAP[current_indx_in_sock_region_map].port_num = handle->sock.fd;
    current_indx_in_sock_region_map++;
    spinlock_unlock(&g_sock_region_map_lock);
    return 0;
}

static struct socket_ops g_tcp_sock_ops = {
    .bind = pal_common_socket_bind,
    .listen = pal_common_tcp_listen,
    .accept = pal_common_tcp_accept,
    .connect = pal_common_socket_connect,
    .send = pal_common_tcp_send,
    .recv = pal_common_tcp_recv,
};

static struct socket_ops g_udp_sock_ops = {
    .bind = pal_common_socket_bind,
    .register_shmem = udp_register_shmem,
    .connect = pal_common_socket_connect,
    .send = pal_common_udp_send,
    .recv = pal_common_udp_recv,
};

static struct handle_ops g_tcp_handle_ops = {
    .attrquerybyhdl = pal_common_socket_attrquerybyhdl,
    .attrsetbyhdl = pal_common_socket_attrsetbyhdl,
    .delete = pal_common_tcp_delete,
    .destroy = pal_common_socket_destroy,
};

static struct handle_ops g_udp_handle_ops = {
    .attrquerybyhdl = pal_common_socket_attrquerybyhdl,
    .attrsetbyhdl = pal_common_socket_attrsetbyhdl,
    .delete = pal_common_udp_delete,
    .destroy = pal_common_socket_destroy,
};
