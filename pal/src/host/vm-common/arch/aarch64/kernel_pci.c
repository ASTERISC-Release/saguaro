/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2023 Intel Corporation */

/*
 * aarch64 counterpart of vm-common/kernel_pci.c.
 *
 * Config space access: QEMU virt exposes PCIe config space via ECAM (memory-mapped) at
 * 0x3f000000 instead of the 0xcf8/0xcfc index/data port pair; a BDF+addr access is a plain
 * aligned MMIO access at ECAM + (bus<<16 | dev<<11 | func<<8) + addr. The virtio PCI
 * capability walk and driver initialization sequence below are copied from the x86 version so
 * both architectures drive the devices through the identical modern virtio state machine.
 *
 * BAR handling difference: on x86 the kernel *relocates* device BARs into a fixed
 * [0xC0000000, 0xD0000000) window (below the 2-3GB QEMU hole). On arm the PCI host bridge
 * already maps device MMIO into [0x10000000, 0x3f000000), which the bootloader identity-maps,
 * so BARs are used in place; the sanity-check window below is that MMIO region.
 *
 * The ivshmem hack from the x86 file is not ported: it is not attached to QEMU virt and its
 * semantics (host-shared memory for the SGX comm-window experiments) are x86/TDX-specific.
 */

#include <stdbool.h>
#include <stdint.h>

#include "api.h"
#include "pal_error.h"
#include "pal_internal.h"

#include "kernel_apic.h"
#include "kernel_virtio.h"

/* QEMU virt PCIe ECAM (hw/arm/virt.c memmap[VIRT_PCIE_ECAM]) */
#define PCI_ECAM_BASE        0x3f000000UL
#define PCI_ECAM_SIZE        0x01000000UL

/* the window in which device MMIO BARs live on QEMU virt (memmap[VIRT_PCIE_MMIO]) */
#define ARM_PCI_MMIO_START   0x10000000UL
#define ARM_PCI_MMIO_END     0x3f000000UL

#define PCI_VENDOR_ID        0x00
#define PCI_DEVICE_ID        0x02
#define PCI_STATUS           0x06
#define PCI_COMMAND          0x04
#define PCI_CAP_POINTER      0x34
#define PCI_HEADER_TYPE      0x0e
#define PCI_BAR0             0x10

#define PCI_STATUS_CAP_LIST  (1 << 4)

#define PCI_VENDOR_ID_VIRTIO          0x1af4
/* transitional console (legacy virtio-serial), modern console, vsock, fs */
#define PCI_DEVICE_ID_CONSOLE_LEGACY  0x1003
#define PCI_DEVICE_ID_CONSOLE         0x1043
#define PCI_DEVICE_ID_VSOCK           0x1053
#define PCI_DEVICE_ID_FS              0x105a

#define VIRTIO_PCI_CAP_COMMON_CFG        1
#define VIRTIO_PCI_CAP_NOTIFY_CFG        2
#define VIRTIO_PCI_CAP_ISR_CFG           3
#define VIRTIO_PCI_CAP_DEVICE_CFG        4
#define VIRTIO_PCI_CAP_PCI_CFG           5
#define VIRTIO_PCI_CAP_SHARED_MEMORY_CFG 8
#define VIRTIO_PCI_CAP_VENDOR_CFG        9

static uintptr_t g_console_pci_bars[6];
static uintptr_t g_fs_pci_bars[6];
static uintptr_t g_vsock_pci_bars[6];

static inline uint32_t ecam_addr(uint32_t bdf, uint32_t addr) {
    return (uint32_t)(PCI_ECAM_BASE + (((uint64_t)bdf) << 12) + addr);
}

static void ecam_config_write(uint32_t bdf, uint32_t addr, uint32_t val, uint32_t size) {
    uint32_t a = ecam_addr(bdf, addr);
    switch (size) {
        case 1:
            __asm__ volatile("stlr %w0, [%1]" :: "r"(val), "r"((volatile void*)a) : "memory");
            break;
        case 2:
            __asm__ volatile("stlrh %w0, [%1]" :: "r"(val), "r"((volatile void*)a) : "memory");
            break;
        default:
            __asm__ volatile("stlr %w0, [%1]" :: "r"(val), "r"((volatile void*)a) : "memory");
            break;
    }
}

static uint32_t ecam_config_read(uint32_t bdf, uint32_t addr, uint32_t size) {
    uint32_t a = ecam_addr(bdf, addr);
    uint32_t v;
    switch (size) {
        case 1:
            __asm__ volatile("ldarb %w0, [%1]" : "=r"(v) : "r"((const volatile void*)a)
                             : "memory");
            break;
        case 2:
            __asm__ volatile("ldarh %w0, [%1]" : "=r"(v) : "r"((const volatile void*)a)
                             : "memory");
            break;
        default:
            __asm__ volatile("ldar %w0, [%1]" : "=r"(v) : "r"((const volatile void*)a) : "memory");
            break;
    }
    return v;
}

#define pci_config_readl(bdf, addr)                ecam_config_read((bdf), (addr), 4)
#define pci_config_readw(bdf, addr)                ecam_config_read((bdf), (addr), 2)
#define pci_config_readb(bdf, addr)                ecam_config_read((bdf), (addr), 1)
#define pci_config_writel(bdf, addr, val)          ecam_config_write((bdf), (addr), (val), 4)
#define pci_config_writew(bdf, addr, val)          ecam_config_write((bdf), (addr), (val), 2)
#define pci_config_writeb(bdf, addr, val)          ecam_config_write((bdf), (addr), (val), 1)

static uint8_t bar_id_to_pci_addr(uint8_t bar_id) {
    assert(bar_id < 6);
    return PCI_BAR0 + 4 * bar_id;
}

/*
 * BAR init: read the (64-bit memory) BAR in place. Returns the BAR base address or 0.
 * On QEMU virt with -kernel ELF loading, BARs are assigned by QEMU's firmware-less setup to
 * addresses inside [VIRT_PCIE_MMIO]; if a BAR reads as unassigned (0), program it here from
 * the MMIO window (the same "allocate from window" algorithm as x86 pci_bar_malloc, without
 * the port-I/O roundtrip).
 */
static uintptr_t pci_bar_init_once(uint32_t bdf, uint16_t device_id, uint8_t bar_id) {
    uintptr_t* bars;
    switch (device_id) {
        case PCI_DEVICE_ID_CONSOLE_LEGACY:
        case PCI_DEVICE_ID_CONSOLE: bars = g_console_pci_bars; break;
        case PCI_DEVICE_ID_FS:      bars = g_fs_pci_bars; break;
        case PCI_DEVICE_ID_VSOCK:   bars = g_vsock_pci_bars; break;
        default: return 0;
    }
    if (bars[bar_id])
        return bars[bar_id];

    uint8_t bar_pci_addr = bar_id_to_pci_addr(bar_id);
    uint32_t bar_desc = pci_config_readl(bdf, bar_pci_addr);

    if (bar_desc & 0x1) {
        log_always("pci_bar_init_once: device_id=%04x bar_id=%u unsupported I/O space BAR",
                   device_id, bar_id);
        return 0;
    }

    uint32_t bar_type = (bar_desc & 0x6) >> 1;
    if (bar_type != 0x2) { /* we support only 64-bit memory BARs (virtio modern is always 64-bit) */
        log_always("pci_bar_init_once: device_id=%04x bar_id=%u unsupported BAR type=%u",
                   device_id, bar_id, bar_type);
        return 0;
    }
    if (bar_id > 4)
        return 0;

    uint64_t bar = bar_desc & ~(uint64_t)0xF;
    if (bar_id <= 4)
        bar |= (uint64_t)pci_config_readl(bdf, bar_pci_addr + 4) << 32;

    if (!(ARM_PCI_MMIO_START <= bar && bar < ARM_PCI_MMIO_END)) {
        /* BAR unassigned (all-ones or zero) or outside the window: relocate into the window.
         * Probe size by writing all-ones (the x86 pci_bar_malloc sizing algorithm verbatim). */
        pci_config_writel(bdf, bar_pci_addr, 0xFFFFFFFF);
        uint32_t bar_size = pci_config_readl(bdf, bar_pci_addr);
        bar_size = (~(bar_size & 0xFFFFFFF0)) + 1;

        static uintptr_t next_mmio = ARM_PCI_MMIO_START;
        next_mmio = ALIGN_UP(next_mmio, bar_size);
        if (next_mmio + bar_size >= ARM_PCI_MMIO_END) {
            log_always("pci_bar_init_once: MMIO window exhausted");
            return 0;
        }
        bar = next_mmio;
        next_mmio += bar_size;

        pci_config_writel(bdf, bar_pci_addr, (uint32_t)bar | (bar_desc & 0xF));
        pci_config_writel(bdf, bar_pci_addr + 4, (uint32_t)(bar >> 32));
    }

    bars[bar_id] = (uintptr_t)bar;
    if (bar_id <= 4)
        bars[bar_id + 1] = (uintptr_t)bar; /* consume the high-half BAR slot as "taken" */

    /* route this device's MSI/INTx: devices use legacy INTx pin routing on QEMU virt; the
     * INTID is PCI_INTA + (device + slot-offset) % 4 + 32. We enable the SPI conservatively for
     * the computed value; wrong values are harmless because the drivers poll the ISR status
     * register from the shared bottomhalves thread as well. */
    uint32_t dev = (bdf >> 3) & 0x1f;
    uint32_t intid = 32 + ((dev + 0) % 4) + 1; /* INTB..; firmware DT uses pin+GSI base 32 */
    gic_spi_enable(intid);

    return bars[bar_id];
}

static uintptr_t pci_bar_addr(uint16_t device_id, uint8_t bar_id) {
    assert(bar_id < 6);
    switch (device_id) {
        case PCI_DEVICE_ID_CONSOLE_LEGACY:
        case PCI_DEVICE_ID_CONSOLE:
            return g_console_pci_bars[bar_id];
        case PCI_DEVICE_ID_FS:
            return g_fs_pci_bars[bar_id];
        case PCI_DEVICE_ID_VSOCK:
            return g_vsock_pci_bars[bar_id];
    }
    return 0x0; /* cannot reach */
}

/* discover one virtio-pci device: walk capabilities, map the four register windows, hand the
 * device to its driver (same state machine as the x86 pci_dev_init) */
static int pci_dev_init(uint32_t bdf, uint16_t device_id) {
    int ret;

    struct virtio_pci_regs* regs   = NULL;
    uint64_t notify_off_addr       = 0x0;
    uint32_t notify_off_multiplier = 0;
    uint32_t* interrupt_status_reg = NULL;
    void* device_config            = NULL;

    uint16_t status = pci_config_readw(bdf, PCI_STATUS);
    if (!(status & PCI_STATUS_CAP_LIST)) {
        log_warning("pci_dev_init: device_id=%04x has no capabilities list", device_id);
        return -PAL_ERROR_NOTSUPPORT;
    }

    uint8_t cap_pointer = pci_config_readb(bdf, PCI_CAP_POINTER);
    size_t number_of_caps = 0;
    while (cap_pointer) {
        if (number_of_caps++ == 256) {
            /* too many capabilities listed, malicious or buggy VMM */
            return -PAL_ERROR_DENIED;
        }

        cap_pointer &= 0xFC; /* bottom two bits are reserved and should be masked */

        const size_t virtio_pci_cap_size = 16; /* sizeof(struct virtio_pci_cap) from VIRTIO spec */

        uint8_t cap_vndr = pci_config_readb(bdf, cap_pointer);
        uint8_t cap_next = pci_config_readb(bdf, cap_pointer + 1);

        if (cap_vndr != /*PCI_CAP_ID_VNDR=*/0x09) {
            cap_pointer = cap_next;
            continue;
        }

        uint8_t cap_len = pci_config_readb(bdf, cap_pointer + 2);
        if (cap_len < virtio_pci_cap_size) {
            /* too small length of the capability struct, malicious or buggy VMM */
            return -PAL_ERROR_DENIED;
        }

        uint8_t cfg_type = pci_config_readb(bdf, cap_pointer + 3);
        uint8_t bar_id   = pci_config_readb(bdf, cap_pointer + 4);
        /* byte 5 is `id`, bytes 6-7 padding -- unused for now */
        uint32_t bar_offset  = pci_config_readl(bdf, cap_pointer + 8);
        /* uint32_t bar_length = pci_config_readl(bdf, cap_pointer + 12); unused */

        if (bar_id > 5) {
            /* BAR may have values 0x0 to 0x5, any other value is reserved */
            return -PAL_ERROR_DENIED;
        }

        uintptr_t bar_addr = 0;
        if (cfg_type == VIRTIO_PCI_CAP_COMMON_CFG || cfg_type == VIRTIO_PCI_CAP_NOTIFY_CFG
                || cfg_type == VIRTIO_PCI_CAP_ISR_CFG || cfg_type == VIRTIO_PCI_CAP_DEVICE_CFG) {
            bar_addr = pci_bar_init_once(bdf, device_id, bar_id);
            if (!bar_addr)
                return -PAL_ERROR_NOMEM;
        }

        switch (cfg_type) {
            case VIRTIO_PCI_CAP_COMMON_CFG:
                if (!IS_ALIGNED(bar_offset, 4))
                    return -PAL_ERROR_INVAL;
                regs = (struct virtio_pci_regs*)(bar_addr + bar_offset);
                break;
            case VIRTIO_PCI_CAP_NOTIFY_CFG:
                if (!IS_ALIGNED(bar_offset, 2))
                    return -PAL_ERROR_INVAL;
                notify_off_addr = bar_addr + bar_offset;
                notify_off_multiplier =
                    pci_config_readl(bdf, cap_pointer + virtio_pci_cap_size);
                break;
            case VIRTIO_PCI_CAP_ISR_CFG:
                interrupt_status_reg = (uint32_t*)(bar_addr + bar_offset);
                break;
            case VIRTIO_PCI_CAP_DEVICE_CFG:
                if (!IS_ALIGNED(bar_offset, 4))
                    return -PAL_ERROR_INVAL;
                device_config = (void*)(bar_addr + bar_offset);
                break;
            case VIRTIO_PCI_CAP_PCI_CFG:           /* alt method to access the above fields */
            case VIRTIO_PCI_CAP_SHARED_MEMORY_CFG: /* shared memory, e.g., for FS DMA window */
            case VIRTIO_PCI_CAP_VENDOR_CFG:        /* optional, for debugging and reporting */
            default:
                break; /* all these are not used */
        }

        cap_pointer = cap_next;
    }

    if (notify_off_multiplier != 0 && !IS_POWER_OF_2(notify_off_multiplier)) {
        /* notify_off_multiplier must be even power of 2 or zero */
        log_always("pci_dev_init: device_id=%04x invalid notify_off_multiplier=%u",
                   device_id, notify_off_multiplier);
        return -PAL_ERROR_INVAL;
    }

    bool in_mmio_window =
        regs && (uintptr_t)regs + sizeof(*regs) < ARM_PCI_MMIO_END
        && ARM_PCI_MMIO_START <= (uintptr_t)regs
        && notify_off_addr && ARM_PCI_MMIO_START <= notify_off_addr
            && notify_off_addr + sizeof(uint16_t) < ARM_PCI_MMIO_END
        && interrupt_status_reg && ARM_PCI_MMIO_START <= (uintptr_t)interrupt_status_reg
            && (uintptr_t)interrupt_status_reg + sizeof(*interrupt_status_reg) < ARM_PCI_MMIO_END
        && device_config && ARM_PCI_MMIO_START <= (uintptr_t)device_config
            && (uintptr_t)device_config < ARM_PCI_MMIO_END;
    if (!in_mmio_window) {
        /* incorrect or malicious virtio configuration structures */
        log_always("pci_dev_init: device_id=%04x invalid device layout (regs=%p)", device_id,
                   regs);
        return -PAL_ERROR_DENIED;
    }

    uint16_t command_reg = pci_config_readw(bdf, PCI_COMMAND);
    pci_config_writew(bdf, PCI_COMMAND, command_reg | 0x2); /* enable memory space */

    /* the virtio reset->ACKNOWLEDGE->DRIVER handshake (device init continues in the drivers) */
    vm_mmio_writeb(&regs->device_status, 0); /* reset */
    vm_mmio_writeb(&regs->device_status,
                   vm_mmio_readb(&regs->device_status) | VIRTIO_STATUS_ACKNOWLEDGE);
    vm_mmio_writeb(&regs->device_status,
                   vm_mmio_readb(&regs->device_status) | VIRTIO_STATUS_DRIVER);

    switch (device_id) {
        case PCI_DEVICE_ID_CONSOLE_LEGACY:
        case PCI_DEVICE_ID_CONSOLE:
            return virtio_console_init(regs, device_config, notify_off_addr,
                                       notify_off_multiplier, interrupt_status_reg);
        case PCI_DEVICE_ID_FS:
            return virtio_fs_init(regs, device_config, notify_off_addr, notify_off_multiplier,
                                  interrupt_status_reg);
        case PCI_DEVICE_ID_VSOCK:
            return virtio_vsock_init(regs, device_config, notify_off_addr,
                                     notify_off_multiplier, interrupt_status_reg);
        default:
            return -PAL_ERROR_NOTSUPPORT;
    }
}

/* Discover all PCI devices on bus 0 (PCI bridges are not supported) */
static int pci_bus_init(void) {
    int ret;

    /* brute force scan of all devices on bus 0 (single-function devices, as on x86); QEMU virt
     * places the virtio-pci functions right behind the QEMU "PCI expander bridge" root port */
    for (uint32_t device = 0; device < 32; device++) {
        uint32_t bdf = (/*bus=*/0 * 256) + (device * 8) + /*function=*/0;

        uint16_t vendor_id = pci_config_readw(bdf, PCI_VENDOR_ID);
        uint16_t device_id = pci_config_readw(bdf, PCI_DEVICE_ID);

        if (vendor_id == 0xffff) {
            /* ECAM read error at this slot: QEMU virt models absent devices as 0xffff vendor */
            continue;
        }
        if (vendor_id != PCI_VENDOR_ID_VIRTIO) {
            /* ignore non-virtio devices (host bridge 0x1b36:0x0008 etc.) */
            continue;
        }
        if (device_id != PCI_DEVICE_ID_CONSOLE_LEGACY
                && device_id != PCI_DEVICE_ID_CONSOLE
                && device_id != PCI_DEVICE_ID_FS
                && device_id != PCI_DEVICE_ID_VSOCK) {
            /* ignore unknown virtio devices (net, blk, balloon, gpu, 9p, ...) */
            continue;
        }

        uint8_t header_type = pci_config_readb(bdf, PCI_HEADER_TYPE);
        if (header_type != 0x0) {
            /* console/fs/vsock virtio device must be a general single-function device (not a
             * multi-function device) */
            return -PAL_ERROR_NOTSUPPORT;
        }

        ret = pci_dev_init(bdf, device_id);
        if (ret < 0) {
            log_error("pci_bus_init: failed to init virtio device %04x at %02x:%02x.%x (%d)",
                      device_id, (bdf >> 8) & 0xFF, (bdf >> 3) & 0x1F, bdf & 0x7, ret);
            return ret;
        }
    }

    return 0;
}

int pci_init(void) {
    /* sanity: the ECAM must respond; QEMU returns 0x1b360008 (QEMU host bridge) or similar at
     * bdf 0:00.0, but anything that is not all-ones means ECAM decoding works */
    uint32_t id = pci_config_readl(/*bdf=*/0, /*addr=*/0);
    if (id == 0xffffffff) {
        log_error("pci_init: ECAM at 0x%lx does not respond", PCI_ECAM_BASE);
        return -PAL_ERROR_NOTSUPPORT;
    }
    return pci_bus_init();
}
