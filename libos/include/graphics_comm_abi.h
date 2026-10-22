/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */
#ifndef GRAPHICS_COMM_ABI_H
#define GRAPHICS_COMM_ABI_H

/*
 * The single definition of the Tirith graphics ABI: the comm page layout, the
 * request ids and the fallback arena geometry. It is shared by the three
 * consumers:
 *
 *   - the guest LibOS (every /dev- and /sys-forwarding syscall), through
 *     libos/include/libos_graphics.h,
 *   - the guest preload library, through libos/src/graphics/common.h,
 *   - the QEMU host listener, whose build resolves this header from the
 *     sibling checkout of this repository (qemu-tirith's tirith-build.sh
 *     looks for it at ../gramine-multiarch, or at GRAMINE_ABI_ROOT, and
 *     exports GRAMINE_ABI_DIR; see qemu-tirith's README.tirith.md).
 *
 * The per-buffer render state (`struct graphics_buffer`, aka `check`) is
 * defined in each tree's own graphics headers (common.h and include/qemu/sg.h)
 * because it names GL/GBM/XCB types that the LibOS does not compile; its layout
 * is part of this ABI and must be kept aligned by hand. Side-specific function
 * declarations and log helpers stay in each tree's own headers accordingly.
 * Changes here are ABI changes: all trees rebuild together (they are
 * versioned together on the dev-graphics branches).
 */

#include <stddef.h>
#include <stdint.h>

/* Request slots: comm_page_t entries at COMM_ADDR + i * sizeof(comm_page_t), each served by its
 * own listener thread, so a guest thread blocked in a long host call (a GPU wait) holds only its
 * slot. They must end before the input ring (SG_INPUT_RING_OFFSET, 512: at most 8 slots). */
#define NUM_COMM_PAGES 4

#define COMM_ADDR  0xf00000ULL
#define COMM_MAGIC 0x1234567812345678ULL

typedef struct {
    volatile uint64_t magic;
    volatile uint64_t req_bit;
    volatile uint64_t ret;
    volatile uint64_t p1;
    volatile uint64_t p2;
    volatile uint64_t p3;
    volatile uint64_t p4;
    volatile uint64_t p5;
} comm_page_t;

_Static_assert(offsetof(comm_page_t, ret) == 16,
               "graphics response must stay in the hot cache line");
_Static_assert(offsetof(comm_page_t, p5) == 56,
               "common graphics payload must fit one cache line");
_Static_assert(sizeof(comm_page_t) == 64,
               "graphics communication ABI must fit one cache line");

/*
 * Guest-private state of the graphics session, in the comm page between the request slots and
 * the input ring (offset 512): the listener never reads this range. The comm page is one
 * physical page that every process of the guest maps, so this is what makes one session serve
 * several processes (dev docs: proton-vm/design-gpu.md, "Several graphics processes"):
 * the first process to turn `session_state` from 0 to 1 runs the setup handshake and publishes
 * the arena geometry it used, then stores 2; the others wait for 2 and only map the arenas. The
 * slot-busy flags are the request-slot locks of the LibOS (libos_graphics_lock.c), shared by the
 * processes instead of per process.
 */
#define GRAPHICS_GUEST_STATE_OFFSET 256

enum {
    GRAPHICS_SESSION_NONE = 0,
    GRAPHICS_SESSION_SETTING_UP = 1,
    GRAPHICS_SESSION_UP = 2,
};

typedef struct {
    volatile uint64_t session_state;
    volatile uint64_t data_addr;
    volatile uint64_t data_size;
    volatile uint64_t huge_addr;
    volatile uint64_t huge_size;
    volatile uint32_t slot_busy[NUM_COMM_PAGES];
} graphics_guest_state_t;

_Static_assert(GRAPHICS_GUEST_STATE_OFFSET >= NUM_COMM_PAGES * sizeof(comm_page_t),
               "the guest state must follow the request slots");
_Static_assert(GRAPHICS_GUEST_STATE_OFFSET + sizeof(graphics_guest_state_t) <= 512,
               "the guest state must end before the input ring");

typedef enum graphics_request {
    LOG_MMAP_EVENT = 1,
    SETUP_DATA = 2,
    GEM_ALLOCATION = 3,
    FSTAT = 4,
    IOCTL = 5,
    OPEN = 6,
    FCNTL = 7,
    READLINK = 8,
    NEWFSTAT = 9,
    GETDENT = 10,
    DUP = 11,
    DUP_IDENTITY = 30,
    X11_SETUP = 12,
    X11_PRESENT = 13,
    X11_IDLE_FENCE_BUFFER = 33,
    CLOSE = 14,
    SYSCALL_LOGGING_ENABLE = 15,
    SYSCALL_LOGGING_DISABLE = 16,
    LSEEK = 22,
    UPDATE_WINDOW_SIZE = 23,
    READ = 24,
    WRITE = 25,
    SYSV_SEMGET = 34,
    SYSV_SEMCTL = 35,
    SYSV_SEMOP = 36,
    HOST_FLOCK = 37,
    HOST_FTRUNCATE = 38,
    UPDATE_INPUT_MASK = 39,
    GEM_FREE = 40, /* p1 = address, p2 = length: the guest unmapped this part of a GEM arena */
} graphics_request_t;

/* IOCTL requests: when p5 holds this value, p4 is (offset + 1) of an int64_t field of the argument
 * that the guest made relative to its own clock (an absolute CLOCK_MONOTONIC deadline minus the
 * guest's "now"); the listener adds its CLOCK_MONOTONIC before the call. Otherwise p4 and p5 are
 * not used by IOCTL (older guests leave other requests' values there, hence the magic). */
#define GRAPHICS_IOCTL_TIME_REL_MAGIC 0x54494d4552454c31ULL /* "TIMEREL1" */

/* The offset between a guest fd and the forwarded host fd of the same object:
 * the guest installs host fds at (host fd + GRAPHICS_FS_OFFSET) and the host
 * listener translates request fd payloads back with the same constant. The
 * application's own fds must stay below it: 500 was too low for a Wine server
 * with a Unity game (dev docs: proton-vm/TESTING.md, 2026-10-16). */
#define GRAPHICS_FS_OFFSET 4096ULL

/* The version of this ABI as the two sides compiled it. The guest writes it to slot 0's p4
 * before the setup magic and the listener answers the magic with its own version in p5; each
 * side stops with a message when they differ, instead of failing later in odd ways (as a
 * mismatched GRAPHICS_FS_OFFSET would). Bump it with every change of the constants, the request
 * ids or the slot layout. 2: GRAPHICS_FS_OFFSET 4096 and this check (2026-10-16). */
#define GRAPHICS_COMM_ABI_VERSION 2ULL

/* Compiled fallbacks for the arena geometry; the runtime values arrive from the
 * launcher (SAGUARO_DATA_ADDR / SAGUARO_DATA_SIZE / SAGUARO_HUGEPAGE_ADDR as
 * environment, the same over the opt/saguaro/graphics_{data_addr,data_size,
 * huge_addr} fw_cfg files), and the guest still ships the geometry to the
 * listener in the comm-page setup (c->p1..p3). */
#define GRAPHICS_GEM_DATA_START      0x100000000ULL
#define GRAPHICS_GEM_DATA_SIZE       (1ULL * 1024 * 1024 * 1024)
#define GRAPHICS_GEM_HUGEPAGE_SIZE   (1ULL * 1024 * 1024 * 1024)

#endif /* GRAPHICS_COMM_ABI_H */