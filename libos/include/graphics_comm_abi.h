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
 *     sibling gramine-multiarch checkout (tirith-build.sh exports
 *     GRAMINE_ABI_DIR; see qemu-tirith's README.tirith.md).
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

#define NUM_COMM_PAGES 1

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
} graphics_request_t;

/* The offset between a guest fd and the forwarded host fd of the same object:
 * the guest installs host fds at (host fd + GRAPHICS_FS_OFFSET) and the host
 * listener translates request fd payloads back with the same constant. */
#define GRAPHICS_FS_OFFSET 500ULL

/* Compiled fallbacks for the arena geometry; the runtime values arrive from the
 * launcher (GRAMINE_DATA_ADDR / GRAMINE_DATA_SIZE / GRAMINE_HUGEPAGE_ADDR as
 * environment, the same over the opt/gramine/graphics_{data_addr,data_size,
 * huge_addr} fw_cfg files), and the guest still ships the geometry to the
 * listener in the comm-page setup (c->p1..p3). */
#define GRAPHICS_GEM_DATA_START      0x100000000ULL
#define GRAPHICS_GEM_DATA_SIZE       (1ULL * 1024 * 1024 * 1024)
#define GRAPHICS_GEM_HUGEPAGE_SIZE   (1ULL * 1024 * 1024 * 1024)

#endif /* GRAPHICS_COMM_ABI_H */