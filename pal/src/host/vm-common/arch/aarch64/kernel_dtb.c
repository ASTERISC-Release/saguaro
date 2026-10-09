/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2023 Intel Corporation */

/*
 * Device Tree blob accessors for the aarch64 unikernel (replacement of the x86 ACPI/e820
 * information channel: QEMU virt passes a DTB at the start of RAM in x0 when booting via
 * -kernel, and it is the authoritative description of memory size, GIC/UART/PCIE addresses
 * and PSCI availability).
 *
 * Minimal big-endian FDT walker per the DevTree flat-dt format: validates the header, walks
 * the structure-block tokens with every access bounded by the header's own totalsize /
 * size_dt_struct / size_dt_strings fields (the blob comes from an untrusted host, so an
 * out-of-bounds token, name or property length must stop the walk rather than read wherever
 * it points). From the walk it extracts exactly what the kernel needs:
 *   - /memory@X reg  -> g_dtb_mem_start / g_dtb_mem_size (the synthesized e820 table),
 *   - /cpus/cpu@N    -> count + the `reg` values, which are the *PSCI CPU_ON target ids*
 *                       (not MPIDRs) that secondary bring-up calls with,
 *   - /psci          -> presence ("arm,psci*" compatible) and conduit (method "hvc" vs the
 *                       default "smc").
 * Node names may carry a unit-address ("memory@40000000"), so name matches are prefix
 * matches up to '@' or NUL. Cell sizes follow the spec defaults (#address-cells=2,
 * #size-cells=1 at the root, 1/0 under /cpus) and honor explicit properties; QEMU virt sets
 * 2/2 at the root but the code must not depend on that.
 */

#include <stdint.h>
#include <string.h>

#include "api.h"

#include "kernel_dtb.h"
#include "kernel_multicore.h"

#define FDT_MAGIC 0xd00dfeedu

/* structure-block tokens (DevTree spec v0.2 ch.5) */
#define FDT_BEGIN_NODE 0x1
#define FDT_END_NODE   0x2
#define FDT_PROP       0x3
#define FDT_NOP        0x0
#define FDT_END        0x9

/* u32 big-endian header field offsets */
#define FDT_HDR_TOTALSIZE    4
#define FDT_HDR_OFF_STRUCT   8
#define FDT_HDR_OFF_STRINGS  12
#define FDT_HDR_SIZE_STRINGS 32
#define FDT_HDR_SIZE_STRUCT  36

static const void* g_dtb = NULL;

uint64_t g_dtb_mem_start = 0;
uint64_t g_dtb_mem_size  = 0;

static uint32_t g_dtb_num_cpus = 0; /* 0 = not found; dtb_num_cpus() floors it to 1 */
static uint32_t g_dtb_cpu_ids[MAX_NUM_CPUS];
static bool g_dtb_psci = false;
static bool g_dtb_psci_hvc = false;

static uint32_t fdt_be32(const void* p) {
    uint32_t v;
    memcpy(&v, p, sizeof(v));
    return __builtin_bswap32(v);
}

static uint64_t fdt_be64(const void* p) {
    uint64_t v;
    memcpy(&v, p, sizeof(v));
    return __builtin_bswap64(v);
}

/* read a big-endian value of `cells` (1 or 2) */
static uint64_t fdt_read_cell(const uint8_t* data, int cells) {
    if (cells == 2) {
        return fdt_be64(data);
    }
    return fdt_be32(data);
}

/* true if DT name equals `literal` ignoring any "@..." unit-address */
static bool name_matches(const char* name, const char* literal) {
    while (*literal && *name == *literal) {
        name++;
        literal++;
    }
    return *literal == '\0' && (*name == '\0' || *name == '@');
}

/* Skip a NUL-terminated, 4-byte-padded name; returns pointer past it or NULL if the
 * structure block ends before the NUL or the padding overruns it. */
static const uint8_t* skip_name(const uint8_t* p, const uint8_t* send) {
    const uint8_t* q = p;
    while (q < send && *q != '\0') {
        q++;
    }
    if (q >= send) {
        return NULL;
    }
    q++; /* NUL */
    q = (const uint8_t*)(((uintptr_t)q + 3) & ~(uintptr_t)3);
    return q > send ? NULL : q;
}

int dtb_init(const void* fdt) {
    if (!fdt) {
        return -1;
    }
    const uint8_t* blob = (const uint8_t*)fdt;
    if (fdt_be32(blob) != FDT_MAGIC) {
        return -1;
    }
    uint32_t totalsize = fdt_be32(blob + FDT_HDR_TOTALSIZE);
    if (totalsize < 40 || totalsize > (1u << 31)) {
        return -1; /* implausible header */
    }
    uint32_t off_struct = fdt_be32(blob + FDT_HDR_OFF_STRUCT);
    uint32_t off_strings = fdt_be32(blob + FDT_HDR_OFF_STRINGS);
    uint32_t size_strings = fdt_be32(blob + FDT_HDR_SIZE_STRINGS);
    uint32_t size_struct = fdt_be32(blob + FDT_HDR_SIZE_STRUCT);

    /* the struct/strings blocks must lie inside the blob we were handed */
    if ((uint64_t)off_struct + size_struct > totalsize
        || (uint64_t)off_strings + size_strings > totalsize) {
        return -1;
    }
    const uint8_t* p = blob + off_struct;
    const uint8_t* send = blob + off_struct + size_struct;
    const char* strings = (const char*)blob + off_strings;

    g_dtb = blob;

    /* One-pass walk with a nesting-depth tracker. Depth counting follows the FDT token
     * structure literally: BEGIN_NODE increments, so the *root* node ("") owns depth 1 and its
     * #address-cells/#size-cells are seen there, while the machine nodes QEMU writes
     * (memory@X, psci, cpus, ...) are depth-2 nodes -- their properties are classified at
     * depth 2 -- and /cpus/cpu@N are depth-3. (The first cut of this parser mistook the
     * root for "depth 0" and saw no /memory at all, rejecting QEMU's perfectly good blob.)
     * `top` is the current depth-2 node name (a pointer into the blob, which stays mapped)
     * and `in_cpus` short-circuits the cpu collection. Per-node flags are reset at
     * BEGIN_NODE of the node that owns the data being collected, so a malformed blob can at
     * worst skip a cpu/memory node -- never merge one node's reg into another's. Note that
     * /cpus also contains cpu-map/clusterN/coreN subtrees without device_type: exactly the
     * device_type=="cpu" check keeps those (whose "cpu" *property* names a phandle) out. */
    int depth = 0;
    const char* top = "";
    int addr_cells = 2;       /* spec default at the root */
    int size_cells = 1;       /* spec default at the root */
    int cpus_addr_cells = 1;  /* spec default under /cpus (QEMU sets 1) */
    bool in_cpus = false;
    /* /memory@X (depth 2) reg state: */
    uint64_t mem_addr = 0, mem_size = 0;
    bool mem_reg = false;
    /* /cpus/cpu@N (depth 3) state: */
    uint64_t cpu_reg = 0;
    bool cpu_reg_seen = false;
    bool cpu_device_type = false;

    while (p + 4 <= send) {
        uint32_t token = fdt_be32(p);
        p += 4;
        if (token == FDT_END) {
            break;
        }
        if (token == FDT_NOP) {
            continue;
        }
        if (token == FDT_BEGIN_NODE) {
            const char* name = (const char*)p;
            p = skip_name(p, send);
            if (!p) {
                return -1;
            }
            depth++;
            if (depth == 2) {
                top = name;
                in_cpus = name_matches(name, "cpus");
                mem_addr = mem_size = 0;
                mem_reg = false;
            } else if (in_cpus && depth == 3) {
                cpu_reg = 0;
                cpu_reg_seen = false;
                cpu_device_type = false;
            }
            continue;
        }
        if (token == FDT_END_NODE) {
            if (in_cpus && depth == 3 && cpu_device_type && cpu_reg_seen
                && g_dtb_num_cpus < MAX_NUM_CPUS) {
                /* cpu node with device_type "cpu" and a reg: reg is its PSCI CPU_ON cpu_id */
                g_dtb_cpu_ids[g_dtb_num_cpus++] = (uint32_t)cpu_reg;
            }
            if (depth == 2) {
                if (name_matches(top, "memory") && mem_reg && mem_addr && mem_size) {
                    g_dtb_mem_start = mem_addr;
                    g_dtb_mem_size = mem_size;
                }
                if (in_cpus) {
                    in_cpus = false;
                }
            }
            depth--;
            continue;
        }
        if (token != FDT_PROP) {
            return -1; /* corrupt structure block */
        }
        uint32_t len = fdt_be32(p);
        uint32_t nameoff = fdt_be32(p + 4);
        p += 8;
        if (len > (uint32_t)(send - p)) {
            return -1;
        }
        const uint8_t* data = p;
        p += (len + 3) & ~3u;
        if (nameoff >= size_strings || strings[nameoff] == '\0') {
            continue; /* property name outside (or unterminated in) the strings block */
        }
        const char* pname = strings + nameoff;

        if (depth == 1 && name_matches(pname, "#address-cells")) {
            addr_cells = (int)fdt_read_cell(data, 1);
        } else if (depth == 1 && name_matches(pname, "#size-cells")) {
            size_cells = (int)fdt_read_cell(data, 1);
        } else if (in_cpus && depth == 2 && name_matches(pname, "#address-cells")) {
            cpus_addr_cells = (int)fdt_read_cell(data, 1);
        } else if (depth == 2 && name_matches(top, "memory") && name_matches(pname, "reg")
                   && len >= (uint32_t)(addr_cells + size_cells) * 4) {
            mem_addr = fdt_read_cell(data, addr_cells);
            mem_size = fdt_read_cell(data + addr_cells * 4, size_cells);
            mem_reg = true;
        } else if (in_cpus && depth == 3 && name_matches(pname, "reg")
                   && len >= (uint32_t)cpus_addr_cells * 4) {
            cpu_reg = fdt_read_cell(data, cpus_addr_cells);
            cpu_reg_seen = true;
        } else if (in_cpus && depth == 3 && name_matches(pname, "device_type")) {
            cpu_device_type = len >= 3 && memcmp(data, "cpu", 3) == 0;
        } else if (depth == 2 && name_matches(top, "psci") && name_matches(pname, "compatible")) {
            /* "arm,psci-1.0"/"arm,psci-0.2"/"arm,psci": any means QEMU exposes the conduit;
             * stored so the SMP bring-up can refuse loudly on a PSCI-less machine. */
            g_dtb_psci = len >= 8 && memcmp(data, "arm,psci", 8) == 0;
        } else if (depth == 2 && name_matches(top, "psci") && name_matches(pname, "method")) {
            g_dtb_psci_hvc = len >= 3 && memcmp(data, "hvc", 3) == 0;
        }
    }

    if (!g_dtb_mem_start || !g_dtb_mem_size) {
        /* No parseable /memory. A QEMU -kernel boot always has one; be loud (pal_main turns
         * this into INIT_FAIL) rather than silently run on stale hard-coded constants. */
        return -1;
    }
    return 0;
}

uint32_t dtb_num_cpus(void) {
    return g_dtb_num_cpus ? g_dtb_num_cpus : 1;
}

uint32_t dtb_cpu_on_id(uint32_t idx) {
    return idx < MAX_NUM_CPUS ? g_dtb_cpu_ids[idx] : 0;
}

bool dtb_psci_available(void) {
    return g_dtb_psci;
}

bool dtb_psci_conduit_hvc(void) {
    return g_dtb_psci_hvc;
}

uint64_t dtb_cntfrq(void) {
    uint64_t freq;
    /* The generic counter frequency is directly readable at EL1 (CNTFRQ_EL0, initialized by
     * QEMU's firmware before entry), so we do not need the DTB /timer node for the common
     * case; keep the DTB path as a TODO fallback. */
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
    return freq;
}
