/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 Gramine contributors */

/*
 * PAL handle definition of the Windows VBS-enclave PAL (the "vbs PAL proper", an AArch64 Linux ELF
 * packed into the signed enclave image; see Documentation/devel/vbs-enclave/PLAN.md).
 */

#pragma once

#ifndef IN_PAL
#error "cannot be included outside PAL"
#endif

#include <stdbool.h>
#include <stdint.h>

#include "spinlock.h"

typedef struct {
    PAL_HDR hdr;
    uint32_t flags;

    union {
        struct {
            const struct pal_vbs_packed_obj* obj; /* a packed file, or NULL for a host file */
            int64_t host_id;      /* host file: the host's id (allowed files), else -1 */
            uint8_t* data;        /* trusted host file: its verified bytes in enclave memory */
            uint64_t size;        /* ... and their number */
            char* path;           /* host file: its path */
        } file;

        struct {
            char* path;      /* normalized absolute path of the directory, no trailing '/' */
            uint64_t cursor; /* index of the next packed entry to look at */
            bool host;       /* also list the host directory (an allowed path) */
            uint64_t host_cursor;
            bool host_done;
        } dir;

        struct {
            int64_t host_id;
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
            bool broadcast;
            bool keepalive;
            bool tcp_cork;
            uint32_t tcp_user_timeout;
            uint32_t tcp_keepidle;
            uint32_t tcp_keepintvl;
            uint8_t tcp_keepcnt;
            bool tcp_nodelay;
            bool ipv6_v6only;
        } sock;

        struct {
            bool writable;
        } console;

        struct {
            struct vbs_chan* rx; /* bytes this end reads */
            struct vbs_chan* tx; /* bytes this end writes */
            bool nonblocking;
        } pipe;

        struct {
            struct vbs_pipe_srv* srv;
        } pipesrv;

        struct {
            struct vbs_eventfd* efd;
            bool nonblocking;
        } eventfd;

        struct {
            PAL_IDX tid;
        } thread;

        struct {
            spinlock_t lock;
            uint32_t signaled;
            bool auto_clear;
        } event;
    };
}* PAL_HANDLE;

#define HANDLE_TYPE(handle) ((handle)->hdr.type)
