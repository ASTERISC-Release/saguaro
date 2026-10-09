/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2023 Intel Corporation */

/*
 * PCI bus.
 *
 * References:
 *   - https://wiki.osdev.org/PCI
 *   - https://people.freebsd.org/~jhb/papers/bsdcan/2007/article/node5.html
 *   - http://www.osdever.net/tutorials/view/multiprocessing-support-for-hobby-oses-explained
 *   - https://forum.osdev.org/viewtopic.php?f=1&t=21745
 *
 * Notes on multi-core synchronization:
 *   - all PCI operations happen only on init, no sync required
 */

#include <stddef.h>
#include <stdint.h>

#include "api.h"
#include "pal_error.h"
#include "pal_internal.h"

#include "kernel_pci.h"
#include "kernel_virtio.h"

static uintptr_t g_console_pci_bars[6];
static uintptr_t g_fs_pci_bars[6];
static uintptr_t g_vsock_pci_bars[6];
static uintptr_t g_ivshmem_pci_bars[6]; /* Adil */

static uint8_t bar_id_to_pci_addr(uint8_t bar_id) {
    assert(bar_id < 6);
    switch (bar_id) {
        case 0: return PCI_BAR0;
        case 1: return PCI_BAR1;
        case 2: return PCI_BAR2;
        case 3: return PCI_BAR3;
        case 4: return PCI_BAR4;
        case 5: return PCI_BAR5;
    }
    return 0; /* cannot reach */
}

static void* pci_malloc(size_t size) {
    static uintptr_t g_pci_mmio_addr = PCI_MMIO_START_ADDR;
    const uintptr_t g_pci_mmio_limit = PCI_MMIO_END_ADDR;

    if (g_pci_mmio_addr >= g_pci_mmio_limit - size)
        return NULL;

    g_pci_mmio_addr = ALIGN_UP(g_pci_mmio_addr, size); /* BARs are aligned on size */
    if (g_pci_mmio_addr >= g_pci_mmio_limit - size)
        return NULL;

    uintptr_t ptr_addr = g_pci_mmio_addr;
    g_pci_mmio_addr += size;
    return (void*)ptr_addr;
}

static void* pci_bar_malloc(uint32_t bdf, uint8_t bar_id, uint32_t bar_desc) {
    assert(bar_id < 5);
    uint8_t bar_pci_addr = bar_id_to_pci_addr(bar_id);

    pci_config_writel(bdf, bar_pci_addr, 0xFFFFFFFF);

    uint32_t bar_size = pci_config_readl(bdf, bar_pci_addr);
    bar_size = (~(bar_size & 0xFFFFFFF0)) + 1;

    /* debug log */
    log_debug("pci_bar_malloc: bdf=%02x:%02x.%x BAR%u size=%u", (bdf >> 8) & 0xFF,
               (bdf >> 3) & 0x1F, bdf & 0x7, bar_id, bar_size);

    void* ptr = pci_malloc(bar_size);
    if (!ptr)
        return NULL;

    pci_config_writel(bdf, bar_pci_addr, ((uint64_t)ptr & 0xFFFFFFFF) | (bar_desc & 0xF));
    pci_config_writel(bdf, bar_pci_addr + 4, (uint64_t)ptr >> 32);

    return ptr;
}

static int pci_console_bar_alloc(uint32_t bdf, uint8_t bar_id, uint32_t bar_desc) {
    assert(bar_id < 5);
    assert(!g_console_pci_bars[bar_id]);

    void* ptr = pci_bar_malloc(bdf, bar_id, bar_desc);
    if (!ptr)
        return -PAL_ERROR_NOMEM;

    g_console_pci_bars[bar_id]     = (uintptr_t)ptr;
    g_console_pci_bars[bar_id + 1] = (uintptr_t)ptr; /* just to make it non-NULL */
    return 0;
}

static int pci_fs_bar_alloc(uint32_t bdf, uint8_t bar_id, uint32_t bar_desc) {
    assert(bar_id < 5);
    assert(!g_fs_pci_bars[bar_id]);

    void* ptr = pci_bar_malloc(bdf, bar_id, bar_desc);
    if (!ptr)
        return -PAL_ERROR_NOMEM;

    g_fs_pci_bars[bar_id]     = (uintptr_t)ptr;
    g_fs_pci_bars[bar_id + 1] = (uintptr_t)ptr; /* just to make it non-NULL */
    return 0;
}

static int pci_vsock_bar_alloc(uint32_t bdf, uint8_t bar_id, uint32_t bar_desc) {
    assert(bar_id < 5);
    assert(!g_vsock_pci_bars[bar_id]);

    void* ptr = pci_bar_malloc(bdf, bar_id, bar_desc);
    if (!ptr)
        return -PAL_ERROR_NOMEM;

    g_vsock_pci_bars[bar_id]     = (uintptr_t)ptr;
    g_vsock_pci_bars[bar_id + 1] = (uintptr_t)ptr; /* just to make it non-NULL */
    return 0;
}

static int pci_ivshmem_bar_alloc(uint32_t bdf, uint8_t bar_id, uint32_t bar_desc) {
    assert(bar_id < 5);
    assert(!g_ivshmem_pci_bars[bar_id]);

    void* ptr = pci_bar_malloc(bdf, bar_id, bar_desc);
    if (!ptr)
        return -PAL_ERROR_NOMEM;

    g_ivshmem_pci_bars[bar_id]     = (uintptr_t)ptr;
    g_ivshmem_pci_bars[bar_id + 1] = (uintptr_t)ptr; /* just to make it non-NULL */
    return 0;
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
        case PCI_DEVICE_ID_IVSHMEM: /* Adil */
            return g_ivshmem_pci_bars[bar_id];
    }
    return 0x0; /* cannot reach */
}

static int pci_bar_init_once(uint32_t bdf, uint16_t device_id, uint8_t bar_id) {
    assert(bar_id < 6);

    if (pci_bar_addr(device_id, bar_id))
        return 0;

    /* BAR not yet allocated and initialized, do it now */
    uint8_t bar_pci_addr = bar_id_to_pci_addr(bar_id);

    uint32_t bar_desc = pci_config_readl(bdf, bar_pci_addr);

    bool bar_io_space = !!(bar_desc & 0x1); /* true - I/O, false - memory */
    if (bar_io_space) {
        /* currently we support only memory-based BARs */
        log_always("pci_bar_init_once: device_id=%04x bar_id=%u unsupported I/O space BAR",
                   device_id, bar_id);
        return -PAL_ERROR_NOTSUPPORT;
    }

    uint32_t bar_type = (bar_desc & 0x6) >> 1;
    if (bar_type != 0x2) {
        /* currently we support only 64-bit memory-based BARs */
        log_always("pci_bar_init_once: device_id=%04x bar_id=%u unsupported BAR type=%u",
                   device_id, bar_id, bar_type);
        return -PAL_ERROR_NOTSUPPORT;
    }

    if (bar_id > 4) {
        /* 64-bit addresses consume 2 BARs, so bar_id cannot be larger than 4 at this point */
        log_always("pci_bar_init_once: device_id=%04x bar_id=%u invalid BAR id for 64-bit BAR",
                   device_id, bar_id);
        return -PAL_ERROR_DENIED;
    }

    switch (device_id) {
        case PCI_DEVICE_ID_CONSOLE_LEGACY:
        case PCI_DEVICE_ID_CONSOLE:
            return pci_console_bar_alloc(bdf, bar_id, bar_desc);
        case PCI_DEVICE_ID_FS:
            return pci_fs_bar_alloc(bdf, bar_id, bar_desc);
        case PCI_DEVICE_ID_VSOCK:
            return pci_vsock_bar_alloc(bdf, bar_id, bar_desc);
        case PCI_DEVICE_ID_IVSHMEM: /* Adil */
            return pci_ivshmem_bar_alloc(bdf, bar_id, bar_desc);
    }

    return -PAL_ERROR_NOTSUPPORT;
}

typedef struct {
    uint32_t magic;        // 'IVSHM'
    uint32_t version;
    uint32_t size;         // Size in bytes (this is what you want!)
    uint32_t peer_id;
    uint32_t max_peers;
    uint32_t reserved[10];
    // ... rest of shared memory follows
} ivshmem_header_t;

/* Adil: initialize ivshmem device */
static int virtio_ivshmem_init(uint32_t bdf) {
    /* Determine BAR2 size */
    uint32_t bar_low = pci_config_readl(bdf, PCI_BAR2);
    pci_config_writel(bdf, PCI_BAR2, 0xFFFFFFFF);
    uint32_t mask_low = pci_config_readl(bdf, PCI_BAR2);
    pci_config_writel(bdf, PCI_BAR2, bar_low); /* restore */

    uint32_t mask_low_clean = mask_low & ~0xFULL;
    uint64_t size = ~(uint64_t)mask_low_clean + 1;

    if ((bar_low & 0x4) != 0) {
        /* 64-bit BAR */
        log_debug("64-bit BAR for ivshmem device");
        uint32_t bar_high = pci_config_readl(bdf, PCI_BAR3);
        pci_config_writel(bdf, PCI_BAR3, 0xFFFFFFFF);
        uint32_t mask_high = pci_config_readl(bdf, PCI_BAR3);
        pci_config_writel(bdf, PCI_BAR3, bar_high); /* restore */

        uint64_t mask = ((uint64_t)mask_high << 32) | mask_low_clean;
        size = ~mask + 1;
    }
    log_debug("virtio_ivshmem_init: bdf=%02x:%02x.%x BAR2 size from mask=%lx",
                (bdf >> 8) & 0xFF, (bdf >> 3) & 0x1F, bdf & 0x7, size);


    // Allocate BAR2 memory
    // uint32_t bar_size = 64 * 1024 * 1024; /* 64KB for ivshmem BAR */
    uint64_t bar_size = size; /* from BAR mask */
    void* ptr = pci_malloc(bar_size);
    if (!ptr) {
        log_warning("virtio_ivshmem_init: failed to allocate %u bytes for ivshmem BAR",
                bar_size);
        return -PAL_ERROR_NOMEM;
    }
    log_debug("virtio_ivshmem_init: allocated %u bytes for ivshmem BAR at %p", bar_size, ptr);

    /* Initialize ivshmem for QEMU */
    uint32_t bar2 = pci_config_readl(bdf, PCI_BAR2);
    uint64_t ptr_val = (uint64_t)ptr;
    uint32_t low_flags = bar2 & 0xFULL;
    if (bar2 & 0x4) {
        /* 64-bit BAR: preserve low flags and write upper 32 bits correctly */
        log_debug("64-bit BAR for ivshmem device");
        pci_config_writel(bdf, PCI_BAR2, (uint32_t)(ptr_val & ~0xFULL)); /* memory space, 32-bit */
        pci_config_writel(bdf, PCI_BAR3, 0);
        log_debug("%x & %x \n", (uint32_t)(ptr_val & ~0xFULL), (uint32_t)ptr_val >> 32);
    } else {
        /* 32-bit BAR */
        log_debug("32-bit BAR for ivshmem device");
        pci_config_writel(bdf, PCI_BAR2, (uint32_t)((ptr_val & ~0xFULL) | low_flags)); /* memory space, 32-bit */
    }

    /* Enable memory spaces only for ivshmem */
    log_debug("IVSHMEM: Enabling memory space for device.");
    uint16_t command_reg = pci_config_readw(bdf, PCI_COMMAND);
    command_reg = (command_reg | 0x2); /* enable Memory space */
    pci_config_writew(bdf, PCI_COMMAND, command_reg);

    return 0;
}

static int pci_dev_init(uint32_t bdf, uint16_t device_id) {
    int ret;

    /* virtio PCI capabilities */
    struct virtio_pci_regs* regs   = NULL;
    uint64_t notify_off_addr       = 0x0;
    uint32_t notify_off_multiplier = 0;
    uint32_t* interrupt_status_reg = NULL;
    void* device_config            = NULL;

    /* Capabilities Pointer only used if bit 4 of the Status reg is set to 1 */
    if (device_id == PCI_DEVICE_ID_IVSHMEM) {
        /* Adil: ivshmem does not have capabilities list */
        log_debug("pci_dev_init: device_id=%04x skipping capabilities list check", device_id);
    } else {
        uint16_t status = pci_config_readw(bdf, PCI_STATUS);
        if (!(status & (1 << 4))) {
            /* no capabilities list, cannot be virtio device */
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
            uint8_t id       = pci_config_readb(bdf, cap_pointer + 5);
            /* next two bytes are padding, skip them */
            uint32_t bar_offset  = pci_config_readl(bdf, cap_pointer + 8);
            uint32_t bar_length  = pci_config_readl(bdf, cap_pointer + 12);

            /* unused for now */
            (void)id;
            (void)bar_length;

            if (bar_id > 5) {
                /* BAR may have values 0x0 to 0x5, any other value is reserved */
                return -PAL_ERROR_DENIED;
            }

            if (cfg_type == VIRTIO_PCI_CAP_COMMON_CFG
                    || cfg_type == VIRTIO_PCI_CAP_NOTIFY_CFG
                    || cfg_type == VIRTIO_PCI_CAP_ISR_CFG
                    || cfg_type == VIRTIO_PCI_CAP_DEVICE_CFG) {
                ret = pci_bar_init_once(bdf, device_id, bar_id);
                if (ret < 0)
                    return ret;
            }

            switch (cfg_type) {
                case VIRTIO_PCI_CAP_COMMON_CFG: {
                    if (!IS_ALIGNED(bar_offset, 4))
                        return -PAL_ERROR_INVAL;
                    uintptr_t bar_addr = pci_bar_addr(device_id, bar_id);
                    regs = (struct virtio_pci_regs*)(bar_addr + bar_offset);
                    break;
                }
                case VIRTIO_PCI_CAP_NOTIFY_CFG: {
                    if (!IS_ALIGNED(bar_offset, 2))
                        return -PAL_ERROR_INVAL;
                    notify_off_addr = pci_bar_addr(device_id, bar_id) + bar_offset;
                    notify_off_multiplier = pci_config_readl(bdf, cap_pointer + virtio_pci_cap_size);
                    break;
                }
                case VIRTIO_PCI_CAP_ISR_CFG: {
                    uintptr_t bar_addr = pci_bar_addr(device_id, bar_id);
                    interrupt_status_reg = (uint32_t*)(bar_addr + bar_offset);
                    break;
                }
                case VIRTIO_PCI_CAP_DEVICE_CFG: {
                    if (!IS_ALIGNED(bar_offset, 4))
                        return -PAL_ERROR_INVAL;
                    uintptr_t bar_addr = pci_bar_addr(device_id, bar_id);
                    device_config = (void*)(bar_addr + bar_offset);
                    break;
                }
                case VIRTIO_PCI_CAP_PCI_CFG:           /* alt method to access the above fields */
                case VIRTIO_PCI_CAP_SHARED_MEMORY_CFG: /* shared memory, e.g., for FS DMA window */
                case VIRTIO_PCI_CAP_VENDOR_CFG:        /* optional, for debugging and reporting */
                default:
                    break; /* all these are not used */
            }

            cap_pointer = cap_next;
        }
    }

    if (device_id == PCI_DEVICE_ID_IVSHMEM) {
      // pci_bar_init_once(bdf, device_id, 0); /* Adil: ivshmem has only BAR0 */
      goto skip_mmio_checks; /* Adil: ivshmem does not have capabilities list */
    }

    if (notify_off_multiplier != 0 && !IS_POWER_OF_2(notify_off_multiplier)) {
        /* notify_off_multiplier must be even power of 2 or zero */
        log_always("pci_dev_init: device_id=%04x invalid notify_off_multiplier=%u",
                   device_id, notify_off_multiplier);
        return -PAL_ERROR_INVAL;
    }

    if (!(PCI_MMIO_START_ADDR <= (uintptr_t)regs &&
            (uintptr_t)regs + sizeof(*regs) < PCI_MMIO_END_ADDR)) {
        /* incorrect or malicious common configuration structure */
        log_always("pci_dev_init: device_id=%04x invalid regs=%p", device_id, regs);
        return -PAL_ERROR_DENIED;
    }
    if (!(PCI_MMIO_START_ADDR <= (uintptr_t)notify_off_addr &&
            (uintptr_t)notify_off_addr + sizeof(uint16_t) < PCI_MMIO_END_ADDR)) {
        /* incorrect or malicious notify offset addr */
        log_always("pci_dev_init: device_id=%04x invalid notify_off_addr=%p", device_id,
                   (void*)notify_off_addr);
        return -PAL_ERROR_DENIED;
    }
    if (!(PCI_MMIO_START_ADDR <= (uintptr_t)interrupt_status_reg &&
            (uintptr_t)interrupt_status_reg + sizeof(*interrupt_status_reg) < PCI_MMIO_END_ADDR)) {
        /* incorrect or malicious interrupt (ISR) status register */
        log_always("pci_dev_init: device_id=%04x invalid interrupt_status_reg=%p", device_id,
                   interrupt_status_reg);
        return -PAL_ERROR_DENIED;
    }
    if (!(PCI_MMIO_START_ADDR <= (uintptr_t)device_config &&
            (uintptr_t)device_config + /*diff devices have diff sizes*/0 < PCI_MMIO_END_ADDR)) {
        /* incorrect or malicious device-specific configuration structure */
        log_always("pci_dev_init: device_id=%04x invalid device_config=%p", device_id,
                   device_config);
        return -PAL_ERROR_DENIED;
    }

skip_mmio_checks:

    /* Adil: ivshmem does not have device status register */
    if (device_id != PCI_DEVICE_ID_IVSHMEM) {
      uint16_t command_reg = pci_config_readw(bdf, PCI_COMMAND);
      pci_config_writew(bdf, PCI_COMMAND, command_reg | 0x3); /* enable Memory and I/O spaces */

      vm_mmio_writeb(&regs->device_status, 0); /* reset */
      vm_mmio_writeb(&regs->device_status,
                    vm_mmio_readb(&regs->device_status) | VIRTIO_STATUS_ACKNOWLEDGE);
      vm_mmio_writeb(&regs->device_status,
                    vm_mmio_readb(&regs->device_status) | VIRTIO_STATUS_DRIVER);
    } else {
      // /* Adil: enabling memory spaces only for ivshmem */
      uint16_t command_reg = pci_config_readw(bdf, PCI_COMMAND);
      command_reg = 0; //(command_reg | 0x2); /* enable Memory space */
      pci_config_writew(bdf, PCI_COMMAND, command_reg);
    }

    switch (device_id) {
        case PCI_DEVICE_ID_CONSOLE_LEGACY:
        case PCI_DEVICE_ID_CONSOLE:
            return virtio_console_init(regs, device_config, notify_off_addr, notify_off_multiplier,
                                       interrupt_status_reg);
        case PCI_DEVICE_ID_FS:
            return virtio_fs_init(regs, device_config, notify_off_addr, notify_off_multiplier,
                                  interrupt_status_reg);
        case PCI_DEVICE_ID_VSOCK:
            return virtio_vsock_init(regs, device_config, notify_off_addr, notify_off_multiplier,
                                     interrupt_status_reg);
        case PCI_DEVICE_ID_IVSHMEM: /* Adil */  
            return virtio_ivshmem_init(bdf);
        default:
            /* Adil */
            log_warning("pci_dev_init: unknown device_id=%04x", device_id);
            return -PAL_ERROR_NOTSUPPORT;
    }

    return 0;
}

/* Discover all PCI devices on bus 0 (PCI bridges are not supported) */
static int pci_bus_init(void) {
    int ret;

    /* brute force scan of all devices (and their functions) on bus 0; note that PCI bus allows up
     * to 32 devices on a single bus, with each device having up to 8 functions, but we only care
     * about single-function devices */
    for (uint32_t device = 0; device < 32; device++) {
        uint32_t bdf = (/*bus=*/0 * 256) + (device * 8) + /*function=*/0;

        uint16_t vendor_id = pci_config_readw(bdf, PCI_VENDOR_ID);
        uint16_t device_id = pci_config_readw(bdf, PCI_DEVICE_ID);
      
        // log_always("Found device %02x:%02x.%x vendor_id=%04x device_id=%04x", 
        //     (bdf >> 8) & 0xFF, (bdf >> 3) & 0x1F, bdf & 0x7, vendor_id, device_id);

        if (vendor_id != PCI_VENDOR_ID_VIRTIO) {
            /* Adil */
            // log_always("Ignoring unknown device %02x:%02x.%x vendor_id=%04x", 
            //     (bdf >> 8) & 0xFF, (bdf >> 3) & 0x1F, bdf & 0x7, vendor_id);
            // log_always("device_id=%04x", device_id);

            /* ignore non-virtio devices */
            continue;
        }

        /* Adil: enabling ivshmem device */
        if (device_id != PCI_DEVICE_ID_CONSOLE_LEGACY &&
                device_id != PCI_DEVICE_ID_CONSOLE &&
                device_id != PCI_DEVICE_ID_FS &&
                device_id != PCI_DEVICE_ID_VSOCK && 
                device_id != PCI_DEVICE_ID_IVSHMEM) { 
            /* ignore unknown virtio devices (only know about console, fs, vsock) */
            log_warning("Ignoring unknown VIRTIO device %02x:%02x.%x vendor_id=%04x", 
                (bdf >> 8) & 0xFF, (bdf >> 3) & 0x1F, bdf & 0x7, vendor_id);  
            log_warning("device_id=%04x", device_id);
            continue;
        }

        uint8_t header_type = pci_config_readb(bdf, PCI_HEADER_TYPE);
        if (header_type != 0x0) {
            /* console/fs/vsock virtio device must be a general single-function device (not a
             * multi-function device) */
            log_warning("Ignoring VIRTIO device %02x:%02x.%x with unsupported header_type=%02x", 
                (bdf >> 8) & 0xFF, (bdf >> 3) & 0x1F, bdf & 0x7, header_type);
            return -PAL_ERROR_NOTSUPPORT;
        }

        /* Virtio initialization logs*/
        log_debug("Initializing VIRTIO device %02x:%02x.%x vendor_id=%04x device_id=%04x", 
            (bdf >> 8) & 0xFF, (bdf >> 3) & 0x1F, bdf & 0x7, vendor_id, device_id);

        ret = pci_dev_init(bdf, device_id);
        if (ret < 0)
            return ret;
    }

    return 0;
}

int pci_init(void) {
    uint32_t id = pci_config_readl(/*bdf=*/0, /*addr=*/0);
    if (id != (PCI_VENDOR_ID_INTEL | (PCI_DEVICE_ID_INTEL_Q35_MCH << 16))) {
        /* only support Q35 machine type (default in QEMU) */
        return -PAL_ERROR_NOTIMPLEMENTED;
    }

    /*
     * Top of Low Usable DRAM: bits 15:4 correspond to bits 31:20, and bits 3:0 are reserved.
     *
     * NOTE: QEMU seems to ignore the TOLUD register (and always assumes TOLUD = 3GB), but we keep
     *       the below write for sanity.
     */
    uint16_t tolud = PCI_MMIO_START_ADDR >> 16;
    pci_config_writew(/*bdf=*/0, PCI_TOLUD, tolud);

    return pci_bus_init();
}
