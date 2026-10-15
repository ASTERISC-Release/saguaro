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
            const struct pal_vbs_packed_obj* obj; /* the packed file (never NULL) */
        } file;

        struct {
            char* path;      /* normalized absolute path of the directory, no trailing '/' */
            uint64_t cursor; /* index of the next packed entry to look at */
        } dir;

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
