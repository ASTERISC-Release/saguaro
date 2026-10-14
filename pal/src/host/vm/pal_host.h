/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2023 Intel Corporation */

#pragma once

#ifndef IN_PAL
#error "cannot be included outside PAL"
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>

#include "pal_common_structs.h"
#define WRAP_MARKER 0xFFFFFFFFu
DEFINE_LIST(pal_handle);

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

typedef struct pal_handle {
    PAL_HDR hdr;
    struct udp_rx_ring *region;
    uintptr_t private_frame;
    /* Number of other processes that hold this same handle object (sent with PalSendHandle). The
     * object lives in PAL memory shared by all process roots; the last holder destroys it, like
     * an open file description shared after fork. */
    uint32_t share_refs;
    /* Bitmask of `PAL_HANDLE_FD_*` flags. */
    uint32_t flags;
    LIST_TYPE(pal_handle) list;

    union {
        /* Common field for accessing underlying host fd. See also `PAL_HANDLE_FD_READABLE`. */
        struct {
            PAL_IDX fd;
        } generic;

        struct pal_handle_inner_console console;
        struct pal_handle_inner_device  dev;
        struct pal_handle_inner_file    file;
        struct pal_handle_inner_dir     dir;
        struct pal_handle_inner_pipe    pipe;
        struct pal_handle_inner_sock    sock;
        struct pal_handle_inner_thread  thread;
        struct pal_handle_inner_event   event;
        struct pal_handle_inner_eventfd eventfd;

        struct {
            uintptr_t shared_stream_frame;
            uint32_t side;
            bool nonblocking;
        } process;
    };
}* PAL_HANDLE;
DEFINE_LISTP(pal_handle);

/* These two flags indicate whether the underlying host fd of `PAL_HANDLE` is readable and/or
 * writable respectively. If none of these is set, then the handle has no host-level fd. */
#define PAL_HANDLE_FD_READABLE  1
#define PAL_HANDLE_FD_WRITABLE  2
/* Set if an error was seen on this handle. Currently only set by `_PalStreamsWaitEvents`. */
#define PAL_HANDLE_FD_ERROR     4

int vm_process_stream_selfcheck(void);
int vm_process_runtime_copy_selfcheck(void);
int vm_process_send_handle(PAL_HANDLE target_process, PAL_HANDLE cargo);
int vm_process_receive_handle(PAL_HANDLE source_process, PAL_HANDLE* out_cargo);

#ifdef __x86_64__
noreturn void pal_start_c(void);
#else /* __aarch64__ */
/* QEMU virt's -kernel ELF entry passes the DTB address in x0; see arch/aarch64/pal_main.c */
noreturn void pal_start_c(const void* fdt);
#endif
