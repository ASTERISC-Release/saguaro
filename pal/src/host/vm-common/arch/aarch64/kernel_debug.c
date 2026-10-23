/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2023 Intel Corporation */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/*
 * Debug prints using VMM's serial port -- aarch64 counterpart of vm-common/kernel_debug.c.
 *
 * QEMU virt's prime cell is an ARM PL011 UART at 0x09000000 (memory-mapped, not the 16550-style
 * port-I/O device x86 has at 0x3f8; the aarch64 Linux kernel even forbids legacy port I/O
 * unless a firmware feature enables it). The rest of this file -- the locking and formatting
 * discipline -- is identical to the x86 version.
 *
 * Note that these functions may be used in very early boot stages when Address Sanitizer is not
 * yet initialized, and thus all functions must be marked with `__attribute_no_sanitize_address`.
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "api.h"
#include "kernel_debug.h"
#include "spinlock.h"

/* QEMU virt PL011 UART0 registers (arm,pl011-compatible). */
#define PL011_BASE       0x09000000UL
#define PL011_DR         (PL011_BASE + 0x000) /* data register (W: transmit, R: receive) */
#define PL011_FR         (PL011_BASE + 0x018) /* flag register */
#define PL011_FR_TXFF    (1u << 5)            /* transmit FIFO full */
#define PL011_CR         (PL011_BASE + 0x300) /* control register */
#define PL011_CR_UARTEN  (1u << 0)

/* "unused" attribute is to silence GCC's "variable defined but not used" on ASan builds */
__attribute__((unused)) static spinlock_t g_debug_serial_io_write_lock = INIT_SPINLOCK_UNLOCKED;

__attribute_no_sanitize_address
static void int_to_str(uint64_t x, char* str, size_t str_size) {
    _real_memset(str, ' ', str_size - 1);

    if (x == 0) {
        str[0] = '0';
        str[1] = 0;
        return;
    }

    size_t i = str_size - 2;
    while (x != 0) {
        str[i--] = (x % 10) + '0';
        x /= 10;
    }
    str[str_size - 1] = 0;
}

static inline uint32_t pl011_readl(uint64_t addr) {
    uint32_t v;
    __asm__ volatile("ldar %w0, [%1]" : "=r"(v) : "r"(addr) : "memory");
    return v;
}

static inline void pl011_writel(uint64_t addr, uint32_t v) {
    __asm__ volatile("stlr %w0, [%1]" :: "r"(v), "r"(addr) : "memory");
}

__attribute_no_sanitize_address
void debug_serial_io_write_char(char c) {
    /* wait for room in the TX FIFO, then hand the character to the UART; QEMU's PL011 model
     * comes enabled out of reset, but re-assert UARTEN defensively for bare -kernel boots. */
    while (pl011_readl(PL011_FR) & PL011_FR_TXFF)
        ;
    pl011_writel(PL011_DR, (uint32_t)(unsigned char)c);
}

__attribute_no_sanitize_address
void debug_serial_io_write(const char* str) {
#ifndef ASAN /* this func may be called before ASan is set up, and spinlock funcs would break */
    spinlock_lock(&g_debug_serial_io_write_lock);
#endif
    for (size_t i = 0; str[i] != '\0'; i++)
        debug_serial_io_write_char(str[i]);
    debug_serial_io_write_char('\n');
#ifndef ASAN
    spinlock_unlock(&g_debug_serial_io_write_lock);
#endif
}

__attribute_no_sanitize_address
void debug_serial_io_write_int(const char* str, uint64_t x) {
    char x_str[32] = {0};
    int_to_str(x, x_str, sizeof(x_str));

#ifndef ASAN /* this func may be called before ASan is set up, and spinlock funcs would break */
    spinlock_lock(&g_debug_serial_io_write_lock);
#endif
    for (size_t i = 0; str[i] != '\0'; i++)
        debug_serial_io_write_char(str[i]);
    for (size_t i = 0; x_str[i] != '\0'; i++)
        debug_serial_io_write_char(x_str[i]);
    debug_serial_io_write_char('\n');
#ifndef ASAN
    spinlock_unlock(&g_debug_serial_io_write_lock);
#endif
}
