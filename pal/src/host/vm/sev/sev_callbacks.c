/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2023 Intel Corporation */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/*
 * VM callbacks of the SEV build of the vm PAL (vm-common reaches the host only through these, see
 * vm_callbacks.h). Plain SEV: as in the vm PAL (../vm_callbacks.c) -- the shared heap and MMIO are
 * mapped unencrypted, port I/O and MSRs are intercepted as usual. SEV-ES/SNP: port I/O, MMIO and
 * the intercepted MSRs go through the GHCB (sev_ghcb.c), so that they need no #VC and no
 * instruction decoding; plain memory accesses to the shared heap stay plain.
 */

#include <stdint.h>

#include "kernel_interrupts.h"
#include "sev.h"
#include "sev_ghcb.h"
#include "vm_callbacks.h"

void* vm_shared_memcpy(void* dest, const void* src, size_t n) {
    return memcpy(dest, src, n);
}

void* vm_shared_memset(void* s, int c, size_t n) {
    return memset(s, c, n);
}

void vm_shared_wrmsr(uint64_t msr, uint64_t value) {
    if (sev_es_active())
        sev_ghcb_wrmsr((uint32_t)msr, value);
    else
        wrmsr(msr, value);
}

int vm_shared_rdmsr(uint64_t msr, uint64_t* out_value) {
    /* a plain RDMSR, as before this callback existed (with SEV-ES, the #VC handler serves it) */
    *out_value = rdmsr(msr);
    return 0;
}

uint8_t vm_shared_readb(uint8_t* shared_addr) {
    return READ_ONCE(*shared_addr);
}

uint16_t vm_shared_readw(uint16_t* shared_addr) {
    return READ_ONCE(*shared_addr);
}

uint32_t vm_shared_readl(uint32_t* shared_addr) {
    return READ_ONCE(*shared_addr);
}

uint64_t vm_shared_readq(uint64_t* shared_addr) {
    return READ_ONCE(*shared_addr);
}

void vm_shared_writeb(uint8_t* shared_addr, uint8_t data) {
    WRITE_ONCE(*shared_addr, data);
    COMPILER_BARRIER();
}

void vm_shared_writew(uint16_t* shared_addr, uint16_t data) {
    WRITE_ONCE(*shared_addr, data);
    COMPILER_BARRIER();
}

void vm_shared_writel(uint32_t* shared_addr, uint32_t data) {
    WRITE_ONCE(*shared_addr, data);
    COMPILER_BARRIER();
}

void vm_shared_writeq(uint64_t* shared_addr, uint64_t data) {
    WRITE_ONCE(*shared_addr, data);
    COMPILER_BARRIER();
}

uint8_t vm_mmio_readb(uint8_t* mmio_addr) {
    if (sev_es_active())
        return (uint8_t)sev_ghcb_mmio_read((uint64_t)mmio_addr, 1);
    return READ_ONCE(*mmio_addr);
}

uint16_t vm_mmio_readw(uint16_t* mmio_addr) {
    if (sev_es_active())
        return (uint16_t)sev_ghcb_mmio_read((uint64_t)mmio_addr, 2);
    return READ_ONCE(*mmio_addr);
}

uint32_t vm_mmio_readl(uint32_t* mmio_addr) {
    if (sev_es_active())
        return (uint32_t)sev_ghcb_mmio_read((uint64_t)mmio_addr, 4);
    return READ_ONCE(*mmio_addr);
}

uint64_t vm_mmio_readq(uint64_t* mmio_addr) {
    if (sev_es_active())
        return sev_ghcb_mmio_read((uint64_t)mmio_addr, 8);
    return READ_ONCE(*mmio_addr);
}

void vm_mmio_writeb(uint8_t* mmio_addr, uint8_t data) {
    if (sev_es_active()) {
        sev_ghcb_mmio_write((uint64_t)mmio_addr, 1, data);
        return;
    }
    vm_shared_writeb(mmio_addr, data);
}

void vm_mmio_writew(uint16_t* mmio_addr, uint16_t data) {
    if (sev_es_active()) {
        sev_ghcb_mmio_write((uint64_t)mmio_addr, 2, data);
        return;
    }
    vm_shared_writew(mmio_addr, data);
}

void vm_mmio_writel(uint32_t* mmio_addr, uint32_t data) {
    if (sev_es_active()) {
        sev_ghcb_mmio_write((uint64_t)mmio_addr, 4, data);
        return;
    }
    vm_shared_writel(mmio_addr, data);
}

void vm_mmio_writeq(uint64_t* mmio_addr, uint64_t data) {
    if (sev_es_active()) {
        sev_ghcb_mmio_write((uint64_t)mmio_addr, 8, data);
        return;
    }
    vm_shared_writeq(mmio_addr, data);
}

uint8_t vm_portio_readb(uint16_t port) {
    if (sev_es_active())
        return (uint8_t)sev_ghcb_ioio_in(port, 1);
    uint8_t val;
    __asm__ volatile("inb %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

uint16_t vm_portio_readw(uint16_t port) {
    if (sev_es_active())
        return (uint16_t)sev_ghcb_ioio_in(port, 2);
    uint16_t val;
    __asm__ volatile("inw %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

uint32_t vm_portio_readl(uint16_t port) {
    if (sev_es_active())
        return sev_ghcb_ioio_in(port, 4);
    uint32_t val;
    __asm__ volatile("inl %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

void vm_portio_writeb(uint16_t port, uint8_t val) {
    if (sev_es_active()) {
        sev_ghcb_ioio_out(port, 1, val);
        return;
    }
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

void vm_portio_writew(uint16_t port, uint16_t val) {
    if (sev_es_active()) {
        sev_ghcb_ioio_out(port, 2, val);
        return;
    }
    __asm__ volatile("outw %0, %1" : : "a"(val), "Nd"(port));
}

void vm_portio_writel(uint16_t port, uint32_t val) {
    if (sev_es_active()) {
        sev_ghcb_ioio_out(port, 4, val);
        return;
    }
    __asm__ volatile("outl %0, %1" : : "a"(val), "Nd"(port));
}

int vm_virtualization_exception(struct isr_regs* regs) {
    /* #VE is Intel TDX; SEV-ES/SNP raise #VC (vector 29, sev_vc.c) */
    __UNUSED(regs);
    BUG();
}
