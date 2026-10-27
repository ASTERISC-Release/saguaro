/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2023 Intel Corporation */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/*
 * aarch64 counterpart of vm/vm_callbacks.c.
 *
 * On x86 these callbacks exist because of Intel TDX: "shared" memory must be accessed through
 * the TDX module's shared-bit mapping, and port I/O has to go through the hypercall/VMM-exit
 * channel. In the plain VM (non-TDX) build they are thin wrappers over ordinary memory and
 * `in`/`out` instructions. On aarch64 there is no shared/private bit and no port I/O at all:
 * "shared" memory is just memory (ld/st with a barrier for cross-CPU visibility), MMIO is
 * device load/store, and the few callers that reference port I/O at all (none in the aarch64
 * build after the fw_cfg/ECAM split) would hit the BUG stub below.
 */

#include <stdint.h>
#include <string.h>

#include "api.h"
#include "pal_error.h"

#include "vm_callbacks.h"

void* vm_shared_memcpy(void* dest, const void* src, size_t n) {
    return memcpy(dest, src, n);
}

void* vm_shared_memset(void* s, int c, size_t n) {
    return memset(s, c, n);
}

void vm_shared_wrmsr(uint64_t msr, uint64_t value) {
    /* No MSRs on aarch64; the only caller is the TDX code path (HOST_TYPE=TDX), which is never
     * built for aarch64 (meson excludes tdx sources). Keep as a link-satisfying trap. */
    __UNUSED(msr);
    __UNUSED(value);
    BUG();
}

int vm_shared_rdmsr(uint64_t msr, uint64_t* out_value) {
    /* No MSRs on aarch64; the only caller is the x86 APIC code. A link-satisfying trap, as above. */
    __UNUSED(msr);
    __UNUSED(out_value);
    BUG();
    return -PAL_ERROR_NOTIMPLEMENTED;
}

/*
 * The vm_shared_* accessors implement plain little-endian loads/stores with release/acquire
 * semantics: the shared memory regions here (virtqueues, pipes, comm rings) are written by
 * both the kernel and another agent (device/DMA or another CPU), so a compiler barrier plus the
 * ARMv8 acquire/release flavors are the correct minimal guarantee -- the same one x86 gets for
 * free from its TSO memory model.
 */
uint8_t vm_shared_readb(uint8_t* shared_addr) {
    uint8_t v;
    __asm__ volatile("ldarb %w0, [%1]" : "=r"(v) : "r"(shared_addr) : "memory");
    return v;
}

uint16_t vm_shared_readw(uint16_t* shared_addr) {
    uint16_t v;
    __asm__ volatile("ldarh %w0, [%1]" : "=r"(v) : "r"(shared_addr) : "memory");
    return v;
}

uint32_t vm_shared_readl(uint32_t* shared_addr) {
    uint32_t v;
    __asm__ volatile("ldar %w0, [%1]" : "=r"(v) : "r"(shared_addr) : "memory");
    return v;
}

uint64_t vm_shared_readq(uint64_t* shared_addr) {
    uint64_t v;
    __asm__ volatile("ldar %0, [%1]" : "=r"(v) : "r"(shared_addr) : "memory");
    return v;
}

void vm_shared_writeb(uint8_t* shared_addr, uint8_t data) {
    __asm__ volatile("stlr %w0, [%1]" :: "r"((uint32_t)data), "r"(shared_addr) : "memory");
}

void vm_shared_writew(uint16_t* shared_addr, uint16_t data) {
    __asm__ volatile("stlrh %w0, [%1]" :: "r"((uint32_t)data), "r"(shared_addr) : "memory");
}

void vm_shared_writel(uint32_t* shared_addr, uint32_t data) {
    __asm__ volatile("stlr %w0, [%1]" :: "r"(data), "r"(shared_addr) : "memory");
}

void vm_shared_writeq(uint64_t* shared_addr, uint64_t data) {
    __asm__ volatile("stlr %0, [%1]" :: "r"(data), "r"(shared_addr) : "memory");
}

uint8_t vm_mmio_readb(uint8_t* mmio_addr) {
    uint8_t v;
    __asm__ volatile("ldarb %w0, [%1]" : "=r"(v) : "r"(mmio_addr) : "memory");
    return v;
}

uint16_t vm_mmio_readw(uint16_t* mmio_addr) {
    uint16_t v;
    __asm__ volatile("ldarh %w0, [%1]" : "=r"(v) : "r"(mmio_addr) : "memory");
    return v;
}

uint32_t vm_mmio_readl(uint32_t* mmio_addr) {
    uint32_t v;
    __asm__ volatile("ldar %w0, [%1]" : "=r"(v) : "r"(mmio_addr) : "memory");
    return v;
}

uint64_t vm_mmio_readq(uint64_t* mmio_addr) {
    uint64_t v;
    __asm__ volatile("ldar %0, [%1]" : "=r"(v) : "r"(mmio_addr) : "memory");
    return v;
}

void vm_mmio_writeb(uint8_t* mmio_addr, uint8_t data) {
    __asm__ volatile("stlr %w0, [%1]" :: "r"((uint32_t)data), "r"(mmio_addr) : "memory");
}

void vm_mmio_writew(uint16_t* mmio_addr, uint16_t data) {
    __asm__ volatile("stlrh %w0, [%1]" :: "r"((uint32_t)data), "r"(mmio_addr) : "memory");
}

void vm_mmio_writel(uint32_t* mmio_addr, uint32_t data) {
    __asm__ volatile("stlr %w0, [%1]" :: "r"(data), "r"(mmio_addr) : "memory");
}

void vm_mmio_writeq(uint64_t* mmio_addr, uint64_t data) {
    __asm__ volatile("stlr %0, [%1]" :: "r"(data), "r"(mmio_addr) : "memory");
}

/*
 * x86 port I/O callbacks: kept only to satisfy the shared header contract. On aarch64 no
 * Saguaro kernel code paths reach these after the fw_cfg (MMIO at 0x09020000) and PCI config
 * (ECAM at 0x3f000000) splits landed; reaching one is a bug.
 */
uint8_t vm_portio_readb(uint16_t port) {
    __UNUSED(port);
    BUG();
}

uint16_t vm_portio_readw(uint16_t port) {
    __UNUSED(port);
    BUG();
}

uint32_t vm_portio_readl(uint16_t port) {
    __UNUSED(port);
    BUG();
}

void vm_portio_writeb(uint16_t port, uint8_t val) {
    __UNUSED(port);
    __UNUSED(val);
    BUG();
}

void vm_portio_writew(uint16_t port, uint16_t val) {
    __UNUSED(port);
    __UNUSED(val);
    BUG();
}

void vm_portio_writel(uint16_t port, uint32_t val) {
    __UNUSED(port);
    __UNUSED(val);
    BUG();
}

int vm_virtualization_exception(struct isr_regs* regs) {
    /* #VE is an Intel TDX concept; there is no analog -- this handler is never registered on
     * the aarch64 exception dispatch (see kernel_interrupts.c). */
    __UNUSED(regs);
    return -PAL_ERROR_NOTIMPLEMENTED;
}
