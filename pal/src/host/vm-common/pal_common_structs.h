/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2023 Intel Corporation */

/*
 * This file contains inner structs that are embedded into `struct pal_handle`.
 * The file is included in `pal_host.h` in the corresponding PAL.
 */

#pragma once

#ifndef IN_PAL
#error "cannot be included outside PAL"
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cpu.h"
#include "list.h"
#include "spinlock.h"

#include "kernel_thread.h"

#define PIPE_NAME_MAX 96

struct pal_handle; /* forward declaration */

struct pal_handle_inner_console {
    PAL_IDX fd;
    spinlock_t lock;
};

struct pal_handle_inner_device { /* unused */
    PAL_IDX fd;
};

struct pal_handle_inner_file {
    uint64_t nodeid;
    uint64_t fh;
    char* realpath;
    bool writable; /* opened for writing */

    /* below fields are used only for trusted files */
    size_t size;
    void*  chunk_hashes; /* array of hashes of file chunks (of type tdx_chunk_hash_t) */
};

struct pal_handle_inner_dir {
    uint64_t nodeid;
    uint64_t fh;
    char* realpath;
    void* buf;
    void* ptr;
    void* end;
    bool endofstream;
};

#ifndef PIPE_BUF_SIZE
#define PIPE_BUF_SIZE PAGE_SIZE   /* also in pal_common.h */
#endif

/* Handles attached to the data at a position of a ring (PalStreamWriteWithHandles): the reader
 * that reaches `pos` gets them, as SCM_RIGHTS fds come with their bytes. `cargo` and `meta` are
 * PAL memory, which every process maps. */
#define PIPE_RING_HANDLE_SLOTS 8
struct pal_pipe_ring_handles {
    uint64_t pos;
    struct pal_handle** cargo;
    size_t count;
    void* meta;
    size_t meta_size;
};

/* One direction of a connection: a ring written by one end and read by the other. */
struct pal_pipe_ring {
    bool       open_write;   /* the writing end is open; else readers get end-of-stream after draining */
    bool       open_read;    /* the reading end is open; else writes fail */
    uint64_t   write_pos;    /* must be always used as buf[write_pos % PIPE_BUF_SIZE] */
    uint64_t   read_pos;     /* must be always used as buf[read_pos % PIPE_BUF_SIZE] */
    int        writer_futex; /* waited on by a writer of a full ring */
    int        reader_futex; /* waited on by a reader of an empty ring */
    uint32_t   hq_head;      /* handle queue, in position order; hq_tail - hq_head entries */
    uint32_t   hq_tail;
    struct pal_pipe_ring_handles hq[PIPE_RING_HANDLE_SLOTS];
    char       buf[PIPE_BUF_SIZE];
};

/* sub-object of pal_handle_inner_pipe: two pipe objects (the two ends of a connection) share one
 * pipe_buf object, which holds one ring per direction (a connection is bidirectional, as a Unix
 * socket; one-way `pipe()`s use one of the rings): ring[0] is written by the connecting end
 * (PAL_TYPE_PIPE) and read by the accepted end (PAL_TYPE_PIPECLI), ring[1] the other way round */
struct pal_handle_inner_pipe_buf {
    spinlock_t lock;
    int        refcount;
    bool       poll_waiting; /* for PalStreamsWaitEvents; protected by lock */
    struct pal_pipe_ring ring[2];
};

struct pal_handle_inner_pipe {
    bool nonblocking;
    char name[PIPE_NAME_MAX];

    /* only for pipesrv type */
    int  connect_futex;
    bool connect_poll_waiting; /* for PalStreamsWaitEvents; protected by g_connecting_pipes_lock */

    /* only for pipe type: closed before a server accepted it; protected by g_connecting_pipes_lock */
    bool closed_pending;

    /* only for pipe/pipecli types -- read/write ends of the pipe */
    struct pal_handle_inner_pipe_buf* pipe_buf; /* protected by g_connecting_pipes_lock */
};

/* The UDP receive ring of a relayed socket (pal_common_sockets.c); every PAL built on vm-common
 * (vm, tdx) keeps a pointer to it in its `pal_handle` as `region`. */
#define WRAP_MARKER 0xFFFFFFFFu
#define DATA_SIZE (1 << 20) /* 1 MiB UDP receive ring */

struct udp_rx_ring {
    // producer writes tail, consumer writes head
    uint32_t head;
    uint32_t tail;
    uint32_t cap;
    uint32_t udp_rx_futex;
    uint32_t sender_port;
    bool relay_connected;
    void *ring;
};

struct pal_handle_inner_sock {
    PAL_IDX fd;
    spinlock_t lock;
    enum pal_socket_domain domain;
    enum pal_socket_type type;
    struct socket_ops* ops;
    uint64_t linger;
    size_t recv_buf_size;
    size_t send_buf_size;
    uint64_t recvtimeout_us;
    uint64_t sendtimeout_us;
    bool is_nonblocking;
    bool reuseaddr;
    bool reuseport;
    bool keepalive;
    bool broadcast;
    bool tcp_cork;
    uint32_t tcp_user_timeout;
    uint32_t tcp_keepidle;
    uint32_t tcp_keepintvl;
    uint8_t tcp_keepcnt;
    bool tcp_nodelay;
    bool ipv6_v6only;
    bool connect_pending;
    bool connect_finished;
    bool connect_direct_vsock;
    uint8_t connect_request[28];
    size_t connect_request_size;
    size_t connect_request_offset;
    uint8_t connect_response[8];
    size_t connect_response_offset;
    int connect_result;
};

struct pal_handle_inner_thread {
    uint32_t tid;
    void*    stack;  /* points to base of stack + fpregs mem region, suitable for free(stack) */
    struct thread* kernel_thread;
};

struct pal_handle_inner_event {
    spinlock_t lock;
    uint32_t waiters_cnt;
    int signaled;
    bool auto_clear;
};

struct pal_handle_inner_eventfd {
    bool nonblocking;
    bool semaphore;
    spinlock_t lock; /* protects below fields */
    uint64_t val;
    int  writer_futex;
    int  reader_futex;
    bool poll_waiting; /* for PalStreamsWaitEvents; protected by lock */
};
