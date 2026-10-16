/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2023 Intel Corporation */

/*
 * This file contains the main function of the PAL loader, which bootstraps the kernel, loads and
 * processes environment, arguments and manifest.
 */

#include <stdint.h>

#include "api.h"
#include "cpu.h"
#include "init.h"
#include "list.h"
#include "pal.h"
#include "pal_common.h"
#include "pal_error.h"
#include "pal_host.h"
#include "pal_internal.h"
#include "pal_rtld.h"
#include "toml.h"
#include "toml_utils.h"

#include "external/fuse_kernel.h"
#include "kernel_apic.h"
#include "kernel_files.h"
#include "kernel_interrupts.h"
#include "kernel_debug.h"
#include "kernel_memory.h"
#include "kernel_multicore.h"
#include "kernel_pci.h"
#include "kernel_sched.h"
#include "kernel_syscalls.h"
#include "kernel_time.h"
#include "kernel_virtio.h"
#include "kernel_vmm_inputs.h"
#include "kernel_xsave.h"
#ifdef PAL_HOST_SEV
#include "sev.h"
#endif

/* the VMM supplied a graphics-arena layout (graphics redirection configured) */
static bool g_graphics_configured;

/* the end of guest RAM (pal_memory.c); memory_address_end of the public state may be extended
 * past it below */
extern void* g_vm_physical_memory_end;

uint64_t g_tsc_mhz;

static struct pal_handle* g_first_thread_handle = NULL;
static e820_table_entry g_e820_entries[E820_NR_ENTRIES];
static size_t g_e820_entries_size;
static const char* g_vm_envp[MAX_ENVS_CNT];

#ifdef __x86_64__
static struct memory_address_space g_initial_process_address_space;
extern char __process_data_start[];
extern char __process_data_end[];
static volatile uint64_t g_process_data_selfcheck __attribute__((section(".process_data")));
static volatile uint64_t g_address_space_selfcheck_page[PAGE_SIZE / sizeof(uint64_t)]
        __attribute__((aligned(PAGE_SIZE), section(".process_data")));

struct address_space_thread_check {
    volatile uint64_t* addr;
    uint64_t observed;
    atomic_bool done;
};

static int address_space_thread_check(void* arg) {
    struct address_space_thread_check* check = arg;
    check->observed = *check->addr;
    *check->addr = 0x7766554433221100UL;
    atomic_store_explicit(&check->done, true, memory_order_release);
    return 0;
}

/* Every process's application memory comes from the frame pool, so it gets the largest free,
 * 1-GiB-aligned guest RAM interval of up to 4 GiB (its identity addresses are reserved from
 * application use). `*out_found` tells whether an interval of `pool_size` existed. */
static int init_multiprocess_frame_pool_sized(uintptr_t pool_size, bool* out_found) {
    extern struct pal_initial_mem_range g_initial_mem_ranges[];

    *out_found = false;
    const uintptr_t search_start = 1UL * 1024 * 1024 * 1024;
    uintptr_t search_end = (uintptr_t)g_pal_public_state.memory_address_end;

    for (size_t e820_idx = 0; e820_idx < g_e820_entries_size / sizeof(g_e820_entries[0]);
            e820_idx++) {
        const e820_table_entry* entry = &g_e820_entries[e820_idx];
        if (entry->type != E820_ADDRESS_RANGE_MEMORY || entry->address > UINTPTR_MAX - entry->size)
            continue;

        uintptr_t range_start = MAX(search_start, (uintptr_t)entry->address);
        uintptr_t range_end = MIN(search_end, (uintptr_t)(entry->address + entry->size));
        const uintptr_t align = 1UL * 1024 * 1024 * 1024;
        uintptr_t candidate = ALIGN_UP(range_start, align);
        for (; candidate <= range_end && pool_size <= range_end - candidate;
                candidate += align) {
            bool overlaps = false;
            for (size_t i = 0; i < g_pal_public_state.initial_mem_ranges_len; i++) {
                if (candidate < g_initial_mem_ranges[i].end &&
                        g_initial_mem_ranges[i].start < candidate + pool_size) {
                    overlaps = true;
                    break;
                }
            }
            if (overlaps)
                continue;

            *out_found = true;
            int ret = pal_add_initial_range(candidate, pool_size, /*prot=*/0,
                                            /*is_user_reserved=*/false, "private_frame_pool");
            if (ret < 0)
                return ret;
            ret = memory_frame_pool_init(candidate, pool_size);
            if (ret < 0)
                return ret;
            ret = pal_enable_shared_frame_slab_allocator();
            if (ret < 0)
                return ret;

            ret = vm_process_stream_selfcheck();
            if (ret < 0)
                return ret;
            log_always("Process stream I/O, endpoint transfer, shutdown, and frame-reclaim "
                       "self-check passed");

            /* PAL thread stacks are intentionally recycled instead of freed from their own
             * stacks. Warm one stack before the frame-reclamation baseline is measured. */
            struct address_space_thread_check warmup_check = {
                .addr = g_address_space_selfcheck_page,
            };
            atomic_init(&warmup_check.done, false);
            PAL_HANDLE warmup_thread;
            ret = pal_common_thread_create_with_address_space(
                    &warmup_thread, address_space_thread_check, &warmup_check,
                    memory_address_space_boot());
            if (ret < 0)
                return ret;
            while (!atomic_load_explicit(&warmup_check.done, memory_order_acquire))
                _PalThreadYieldExecution();
            while (!sched_thread_is_stopped(warmup_thread->thread.kernel_thread))
                _PalThreadYieldExecution();
            bool warmup_succeeded = warmup_check.observed == 0 &&
                                    *g_address_space_selfcheck_page ==
                                            0x7766554433221100UL;
            PalObjectDestroy(warmup_thread);
            *g_address_space_selfcheck_page = 0;
            if (!warmup_succeeded)
                return -PAL_ERROR_DENIED;

            ret = vm_process_runtime_copy_selfcheck();
            if (ret < 0)
                return ret;

            size_t initial_frame_count = memory_frame_allocated_count();
            uintptr_t frame1, frame2, reused_frame;
            ret = memory_frame_alloc(&frame1);
            if (ret < 0) {
                log_error("frame-pool self-check allocation 1 failed: %s", pal_strerror(ret));
                return ret;
            }
            ret = memory_frame_alloc(&frame2);
            if (ret < 0) {
                memory_frame_free(frame1);
                log_error("frame-pool self-check allocation 2 failed: %s", pal_strerror(ret));
                return ret;
            }
            if (frame1 == frame2 || !IS_ALIGNED_POW2(frame1, PAGE_SIZE) ||
                    !IS_ALIGNED_POW2(frame2, PAGE_SIZE)) {
                memory_frame_free(frame1);
                memory_frame_free(frame2);
                return -PAL_ERROR_DENIED;
            }
            *(uint64_t*)memory_frame_pointer(frame1) = 0xfeedfacecafebeefUL;
            ret = memory_frame_free(frame1);
            if (ret < 0) {
                memory_frame_free(frame2);
                log_error("frame-pool self-check free failed: %s", pal_strerror(ret));
                return ret;
            }
            ret = memory_frame_free(frame1);
            if (ret != -PAL_ERROR_INVAL) {
                memory_frame_free(frame2);
                log_error("frame-pool self-check double-free returned %s", pal_strerror(ret));
                return -PAL_ERROR_DENIED;
            }
            ret = memory_frame_alloc(&reused_frame);
            if (ret < 0) {
                memory_frame_free(frame2);
                return ret;
            }
            log_always("Frame-pool allocate, double-free rejection, and frame-reuse self-check passed");
            if (reused_frame != frame1 || *(uint64_t*)memory_frame_pointer(reused_frame) != 0) {
                memory_frame_free(frame2);
                memory_frame_free(reused_frame);
                return -PAL_ERROR_DENIED;
            }

            uintptr_t test_addr = (uintptr_t)g_address_space_selfcheck_page;
            volatile uint64_t* parent_value = (volatile uint64_t*)test_addr;
            uint64_t original_value = *parent_value;
            const uint64_t parent_marker = 0x1122334455667788UL;
            const uint64_t child_marker = 0x8877665544332211UL;
            *parent_value = parent_marker;
            uint64_t original_process_data_value = g_process_data_selfcheck;
            g_process_data_selfcheck = parent_marker;

            /* Once mapped with memory_address_space_map_owned(), a frame belongs to the leaf and
             * is released by the address space's destroy. */
            bool reused_owned = false, frame2_owned = false;
            struct memory_address_space child_space = {0};
            struct memory_address_space grandchild_space = {0};
            ret = memory_address_space_clone(g_pml4_table_base, &child_space);
            if (ret == 0) {
                ret = memory_address_space_copy_private_range(
                        memory_address_space_boot(), &child_space,
                        (uintptr_t)__process_data_start,
                        (uintptr_t)__process_data_end - (uintptr_t)__process_data_start);
            }
            if (ret == 0) {
                ret = memory_address_space_map_owned(&child_space, test_addr, reused_frame,
                                                     /*write=*/true, /*execute=*/false,
                                                     /*usermode=*/true);
                reused_owned = ret == 0;
            }
            if (ret < 0) {
                *parent_value = original_value;
                log_error("Address-space child clone/map self-check failed: %s",
                          pal_strerror(ret));
                memory_address_space_destroy(&child_space);
                if (!frame2_owned)
                    memory_frame_free(frame2);
                if (!reused_owned)
                    memory_frame_free(reused_frame);
                return ret;
            }

            ret = memory_address_space_switch(&child_space);
            if (ret < 0) {
                *parent_value = original_value;
                log_error("Address-space child switch self-check failed: %s",
                          pal_strerror(ret));
                memory_address_space_destroy(&child_space);
                if (!frame2_owned)
                    memory_frame_free(frame2);
                if (!reused_owned)
                    memory_frame_free(reused_frame);
                return ret;
            }
            uint64_t child_initial_value = *(volatile uint64_t*)test_addr;
            *(volatile uint64_t*)test_addr = child_marker;
            bool child_process_data_initial = g_process_data_selfcheck == parent_marker;
            g_process_data_selfcheck = child_marker;

            const uint64_t grandchild_marker = 0x1029384756abcdefUL;
            ret = memory_address_space_clone_from(&child_space, &grandchild_space);
            if (ret == 0)
                ret = memory_address_space_copy_private_range(
                        &child_space, &grandchild_space, (uintptr_t)__process_data_start,
                        (uintptr_t)__process_data_end - (uintptr_t)__process_data_start);
            if (ret == 0) {
                ret = memory_address_space_map_owned(&grandchild_space, test_addr, frame2,
                                                     /*write=*/true, /*execute=*/false,
                                                     /*usermode=*/true);
                frame2_owned = ret == 0;
            }
            if (ret < 0) {
                memory_address_space_switch_root(g_pml4_table_base);
                log_error("Address-space grandchild clone/map self-check failed: %s",
                          pal_strerror(ret));
                memory_address_space_destroy(&grandchild_space);
                memory_address_space_destroy(&child_space);
                if (!frame2_owned)
                    memory_frame_free(frame2);
                if (!reused_owned)
                    memory_frame_free(reused_frame);
                return ret;
            }

            ret = memory_address_space_switch(&grandchild_space);
            if (ret < 0) {
                memory_address_space_switch_root(g_pml4_table_base);
                log_error("Address-space grandchild switch self-check failed: %s",
                          pal_strerror(ret));
                memory_address_space_destroy(&grandchild_space);
                memory_address_space_destroy(&child_space);
                if (!frame2_owned)
                    memory_frame_free(frame2);
                if (!reused_owned)
                    memory_frame_free(reused_frame);
                return ret;
            }
            bool grandchild_isolated = *(volatile uint64_t*)test_addr == 0 &&
                                       g_process_data_selfcheck == child_marker;
            *(volatile uint64_t*)test_addr = grandchild_marker;
            g_process_data_selfcheck = grandchild_marker;

            struct memory_address_space parent_space = {
                .root = memory_frame_pointer(g_pml4_table_base),
                .root_phys = g_pml4_table_base,
                .base_root = memory_frame_pointer(g_pml4_table_base),
            };
            ret = memory_address_space_switch(&parent_space);
            if (ret < 0) {
                memory_address_space_switch_root(g_pml4_table_base);
                log_error("Address-space parent switch self-check failed: %s",
                          pal_strerror(ret));
                memory_address_space_destroy(&grandchild_space);
                memory_address_space_destroy(&child_space);
                if (!frame2_owned)
                    memory_frame_free(frame2);
                if (!reused_owned)
                    memory_frame_free(reused_frame);
                return ret;
            }

            bool isolated = child_initial_value == 0 && child_process_data_initial &&
                            grandchild_isolated &&
                            g_process_data_selfcheck == parent_marker &&
                            *parent_value == parent_marker &&
                            *(volatile uint64_t*)memory_frame_pointer(reused_frame) == child_marker &&
                            *(volatile uint64_t*)memory_frame_pointer(frame2) == grandchild_marker;
            if (!isolated)
                log_error("Address-space isolation failed: child=%lx pdata=%d grandchild=%d parent=%lx child-frame=%lx grandchild-frame=%lx",
                          child_initial_value, child_process_data_initial, grandchild_isolated,
                          *parent_value, *(volatile uint64_t*)memory_frame_pointer(reused_frame),
                          *(volatile uint64_t*)memory_frame_pointer(frame2));
            g_process_data_selfcheck = original_process_data_value;
            if (!isolated) {
                *parent_value = original_value;
                memory_address_space_destroy(&grandchild_space);
                memory_address_space_destroy(&child_space);
                if (!frame2_owned)
                    memory_frame_free(frame2);
                if (!reused_owned)
                    memory_frame_free(reused_frame);
                return -PAL_ERROR_DENIED;
            }

            /* A descendant owns a full private table tree and must survive its parent's teardown. */
            memory_address_space_destroy(&child_space);
            ret = memory_address_space_switch(&grandchild_space);
            if (ret < 0) {
                memory_address_space_switch_root(g_pml4_table_base);
                log_error("Address-space grandchild resume self-check failed: %s",
                          pal_strerror(ret));
                memory_address_space_destroy(&grandchild_space);
                if (!frame2_owned)
                    memory_frame_free(frame2);
                if (!reused_owned)
                    memory_frame_free(reused_frame);
                return ret;
            }
            bool grandchild_survived_parent =
                    *(volatile uint64_t*)test_addr == grandchild_marker &&
                    g_process_data_selfcheck == grandchild_marker;
            ret = memory_address_space_switch_root(g_pml4_table_base);
            if (ret < 0) {
                memory_address_space_destroy(&grandchild_space);
                if (!frame2_owned)
                    memory_frame_free(frame2);
                if (!reused_owned)
                    memory_frame_free(reused_frame);
                return ret;
            }

            bool scheduler_switched = true;
            {
                struct address_space_thread_check thread_check = {
                    .addr = (volatile uint64_t*)test_addr,
                };
                atomic_init(&thread_check.done, false);
                PAL_HANDLE thread_handle;
                ret = pal_common_thread_create_with_address_space(
                        &thread_handle, address_space_thread_check, &thread_check,
                        &grandchild_space);
                if (ret < 0) {
                    *parent_value = original_value;
                    log_error("Address-space scheduler-thread create self-check failed: %s",
                              pal_strerror(ret));
                    memory_address_space_destroy(&grandchild_space);
                    if (!frame2_owned)
                    memory_frame_free(frame2);
                    if (!reused_owned)
                    memory_frame_free(reused_frame);
                    return ret;
                }

                while (!atomic_load_explicit(&thread_check.done, memory_order_acquire))
                    _PalThreadYieldExecution();
                while (!sched_thread_is_stopped(thread_handle->thread.kernel_thread))
                    _PalThreadYieldExecution();

                scheduler_switched = thread_check.observed == grandchild_marker &&
                                     *parent_value == parent_marker &&
                                     *(volatile uint64_t*)memory_frame_pointer(frame2) ==
                                             0x7766554433221100UL;
                if (!scheduler_switched)
                    log_error("Scheduler root check values: observed=%lx parent=%lx child-frame=%lx grandchild-frame=%lx",
                              thread_check.observed, *parent_value,
                              *(volatile uint64_t*)memory_frame_pointer(reused_frame),
                              *(volatile uint64_t*)memory_frame_pointer(frame2));
                PalObjectDestroy(thread_handle);
            }
            *parent_value = original_value;
            memory_address_space_destroy(&grandchild_space);
            /* both frames were released with their address spaces */
            if (memory_frame_allocated_count() != initial_frame_count)
                log_error("Address-space frame leak: started %zu, ended %zu",
                          initial_frame_count, memory_frame_allocated_count());
            if (memory_frame_allocated_count() != initial_frame_count)
                return -PAL_ERROR_DENIED;
            if (!scheduler_switched || !grandchild_survived_parent)
                log_error("Address-space teardown/scheduler failed: scheduler=%d descendant=%d",
                          scheduler_switched, grandchild_survived_parent);
            if (!scheduler_switched || !grandchild_survived_parent)
                return -PAL_ERROR_DENIED;
            log_always("Address-space isolation, nested clone, parent teardown, frame reclaim, and scheduler self-check passed");
            return 0;
        }
    }
    return -PAL_ERROR_NOMEM;
}

static int init_multiprocess_frame_pool(void) {
    for (uintptr_t gib = 4; gib >= 1; gib /= 2) {
        bool found;
        int ret = init_multiprocess_frame_pool_sized(gib * 1024 * 1024 * 1024, &found);
        if (found)
            return ret;
    }
    return -PAL_ERROR_NOMEM;
}
#endif

#define FW_CFG_RAM_SIZE 0x03 /* 64-bit RAM size in bytes */
#define FW_CFG_NB_CPUS  0x05 /* 32-bit number of vCPUs */

/* works for little-endian variables of all bit sizes */
static uint64_t rdfwcfg(uint64_t cfg, size_t size) {
    short port; /* unused, added for sanity */
    size_t len; /* unused, added for sanity */
    __asm__ volatile(
        "   out  %w0, %1\n"
        "   inc  %1\n"
        "1: in   %1, %b0\n"
        "   ror  $8, %0\n"
        "   loop 1b\n"
        : "+a"(cfg), "=d"(port), "=c"(len)
        : "1"(FW_CFG_PORT_SEL), "2"(size)
        );
    cfg = cfg >> (8 * (sizeof(uint64_t) - size));
    return cfg;
}

static int tsc_frequency_init(void) {
    uint32_t words[CPUID_WORD_NUM];

    /*
     * We rely on the "invariant TSC" hardware feature (bit CPUID.80000007H:EDX[8]).
     * We also rely on the Generic CPUID space for hypervisors (QEMU/KVM in particular):
     *   - 0x40000000: EAX: The maximum input value for CPUID supported by the hypervisor
     *   - 0x40000010: EAX: (Virtual) TSC frequency in kHz
     */
    _PalCpuIdRetrieve(INVARIANT_TSC_LEAF, 0, words);
    if (!(words[CPUID_WORD_EDX] & (1 << 8))) {
        /* invariant TSC is not available */
        return -PAL_ERROR_DENIED;
    }

    _PalCpuIdRetrieve(0x40000000, 0, words);
    if (words[CPUID_WORD_EAX] < 0x40000010) {
        /* virtual TSC frequency is not available */
        return -PAL_ERROR_DENIED;
    }

    _PalCpuIdRetrieve(0x40000010, 0, words);
    uint64_t tsc_mhz = words[CPUID_WORD_EAX] / 1000;
    if (!tsc_mhz)
        return -PAL_ERROR_DENIED;

    g_tsc_mhz = tsc_mhz;
    return 0;
}

static int switch_apic_to_x2_mode(void) {
    uint32_t words[CPUID_WORD_NUM];
    cpuid(FEATURE_FLAGS_LEAF, 0, words);
    if (!(words[CPUID_WORD_ECX] & (1 << 21))) {
        /* x2APIC mode is not available */
        return -PAL_ERROR_DENIED;
    }

    uint64_t msr = rdmsr(MSR_IA32_APIC_BASE);
    msr |= (1 << 10) + (1 << 11); /* bit 10 -- Enable x2APIC mode; bit 11 -- xAPIC global enable */
    wrmsr(MSR_IA32_APIC_BASE, msr);
    return 0;
}

static int add_preloaded_range(uintptr_t addr, size_t size, bool is_user_reserved,
                               const char* comment) {
    return pal_add_initial_range(addr, size, /*pal_prot=*/0, is_user_reserved, comment);
}

static void prot_none_free_memory(void) {
    extern struct pal_initial_mem_range g_initial_mem_ranges[];

    /* Free guest RAM is inaccessible (PROT_NONE in the guest page tables) until
     * the allocator hands pages out. The pages are deliberately NOT written
     * zero here: the memfd (and hugetlbfs) backends are zero by construction,
     * anything the boot wrote lives in the reserved ranges skipped below, and
     * later allocations re-zero through memory_alloc(). Writing zero into every
     * free page used to cost a whole-guest-RAM memset and committed all of it
     * physically, defeating lazy allocation of the "rest" backends. */
    uint64_t cur_mem_range_idx = 0;
    uintptr_t addr = (uintptr_t)g_pal_public_state.memory_address_end;
    while (true) {
        if (!addr)
            return;
        addr -= PAGE_SIZE;

        while (cur_mem_range_idx < g_pal_public_state.initial_mem_ranges_len
                && addr < g_initial_mem_ranges[cur_mem_range_idx].start) {
            /* skip too-high mem ranges; we rely on mem ranges to be sorted in desc order */
            cur_mem_range_idx++;
        }

        while (cur_mem_range_idx < g_pal_public_state.initial_mem_ranges_len
                && addr >= g_initial_mem_ranges[cur_mem_range_idx].start
                && addr <  g_initial_mem_ranges[cur_mem_range_idx].end) {
            /* jump over a mem range; we rely on mem ranges to be sorted in desc order */
            addr = g_initial_mem_ranges[cur_mem_range_idx].start;
            if (!addr)
                return;
            addr -= PAGE_SIZE;
            cur_mem_range_idx++;
        }

        if (addr < (uintptr_t)g_pal_public_state.memory_address_start)
            return;

        int ret = memory_protect((void*)addr, PAGE_SIZE, /*read=*/false, /*write=*/false,
                                 /*execute=*/false);
        if (ret < 0)
            BUG();
    }
}

noreturn static void print_usage_and_exit(void) {
    log_always("USAGE: init <application> args...");
    log_always("This is an internal interface. Use gramine-vm to launch applications.");
    _PalProcessExit(1);
}

noreturn int pal_start_continue(void* cmdline_);

/* Boot-time instrumentation: relative stamps against the first instruction of the
 * kernel, in TSC ticks and (once the TSC frequency is known) milliseconds. Also routed
 * through the debug serial port: the console (virtio) only carries output written after
 * its driver is up, so everything before that is invisible there (see Documentation/devel/graphics/FINDINGS.md). */
static uint64_t boot_tsc0;
static void boot_stamp(const char* what) {
    uint64_t t = get_tsc() - boot_tsc0;
    if (g_tsc_mhz) {
        unsigned long ms_val = (unsigned long)(t / (g_tsc_mhz * 1000));
        log_always("[boot] +%lu ms: %s", ms_val, what);
        debug_serial_io_write("[boot] +");
        debug_serial_io_write_int("", ms_val);
        debug_serial_io_write(" ms: ");
        debug_serial_io_write(what);
        debug_serial_io_write("\n");
    } else {
        log_always("[boot] +%llu ticks: %s", (unsigned long long)t, what);
        debug_serial_io_write("[boot] ");
        debug_serial_io_write_int("", t);
        debug_serial_io_write(" ticks: ");
        debug_serial_io_write(what);
        debug_serial_io_write("\n");
    }
}

/* called by `vm_bootloader.S` on kernel startup */
__attribute_no_stack_protector  /* starts with zero GS base register */
__attribute_no_sanitize_address /* starts with dummy page tables w/o ASan shared memory */
noreturn void pal_start_c(void) {
    int ret;
    boot_tsc0 = get_tsc();

    set_dummy_gs_base();

    /* initialize alloc_align as early as possible, a lot of PAL APIs depend on this being set */
    g_pal_public_state.alloc_align = PAGE_SIZE;
    assert(IS_POWER_OF_2(g_pal_public_state.alloc_align));

#ifdef PAL_HOST_SEV
    /* first: under SEV-ES/SNP every port I/O, MSR and CPUID below needs the GHCB and #VC handler */
    sev_init_early();
#endif

    uint32_t num_cpus = rdfwcfg(FW_CFG_NB_CPUS, sizeof(uint32_t));
    if (num_cpus < 1 || num_cpus > MAX_NUM_CPUS)
        INIT_FAIL("Detected unsupported number of virtual CPUs: %u (supported: 1..%u)", num_cpus,
                  MAX_NUM_CPUS);

    ret = e820_table_init((char*)g_e820_entries, &g_e820_entries_size,
                          sizeof(g_e820_entries));
    if (ret < 0)
        INIT_FAIL("Can't read E820 table from VMM");

    ret = memory_init(g_e820_entries, g_e820_entries_size,
                      &g_pal_public_state.memory_address_start,
                      &g_pal_public_state.memory_address_end);
    if (ret < 0)
        INIT_FAIL("Failed to initialize physical memory");
    g_vm_physical_memory_end = g_pal_public_state.memory_address_end;
#ifdef __x86_64__
    /* untrusted_shm mappings (dev:/dev/shm) live in ordinary guest memory, as on the Linux PAL */
    g_pal_public_state.shared_address_start = g_pal_public_state.memory_address_start;
    g_pal_public_state.shared_address_end = g_pal_public_state.memory_address_end;
#endif

    /* vm_bootloader.S installed tiny page tables that cover [0..32MB) of RAM */
    ret = memory_pagetables_init(g_pal_public_state.memory_address_end,
                                 /*current_page_tables_cover_1gb=*/false);
    if (ret < 0)
        INIT_FAIL("Failed to initialize page tables");

    ret = memory_preload_ranges(g_e820_entries, g_e820_entries_size, &add_preloaded_range);
    if (ret < 0)
        INIT_FAIL("Failed to initialize preloaded ranges");

    /* Graphics GEM arenas: geometry from the VMM (opt/gramine/graphics_data_size and
     * opt/gramine/graphics_huge_addr, absent without graphics). Reserved NOW -- before zero_out
     * and all kernel init -- so that no boot-time allocation can ever claim pages inside the
     * arenas. The manifest itself is only readable once virtio-fs is up, so the geometry travels
     * through fw_cfg instead. */
    uint64_t graphics_data_size = 0;
    uint64_t graphics_huge_addr = 0;
    g_graphics_configured = false;
    if (graphics_data_size_init(&graphics_data_size) == 0 && graphics_data_size) {
        uint64_t graphics_data_addr = 0;
        graphics_data_addr_init(&graphics_data_addr);  /* optional; 0 = the compiled 4G anchor */
        graphics_huge_addr_init(&graphics_huge_addr); /* optional; 0 = follow the data arena */
        graphics_arenas_set_geometry((uintptr_t)graphics_data_addr, (size_t)graphics_data_size,
                                     (uintptr_t)graphics_huge_addr);
        g_graphics_configured = true;
        boot_stamp("graphics arenas reserved");
    }

    ret = memory_reserve_graphics_arenas(&add_preloaded_range);
    if (ret < 0)
        INIT_FAIL("Failed to reserve the graphics arenas (arena size or VM memory size)");

    boot_stamp("initial ranges set");

    /* PAL binary is located at 1MB and may occupy until 4MB, see pal.lds */
    /* FIXME: whole PAL binary is RWX because memory_pagetables_init() marked everything as RWX and
     *        prot_none_free_memory() did *not* modify perms for PAL binary memory pages */
    ret = add_preloaded_range(0x100000UL, 0x300000UL, /*is_user_reserved=*/false, "pal_binary");
    if (ret < 0)
        INIT_FAIL("Failed to preload PAL-binary memory range");

    /* Common memory-allocation logic relies on all memory pages to be zeroed out after boot.
     * This is not true for common hypervisors like QEMU/KVM, so must do it ourselves. Also,
     * memory_pagetables_init() marked all pages as RWX, now is good time to revert to NONE. */
    /* Free guest RAM is inaccessible until the allocator hands pages out. See
     * prot_none_free_memory() for why the old whole-RAM zero-fill is gone. */
    prot_none_free_memory();
    boot_stamp("free memory PROT_NONE");

    /* Configure PAT so that index 1 is Write-Combining (WC). Must be after multicore init because
     * PAT is per-CPU state and AP CPUs will also need it set. */
    memory_pat_init_for_wc();

    call_init_array();

    init_slab_mgr();

    /* must be before apic_init() since the latter uses RDTSC */
    ret = tsc_frequency_init();
    if (ret < 0)
        INIT_FAIL("Failed to initialize TSC frequency based on Invariant TSC HW feature");

    ret = time_init();
    if (ret < 0)
        INIT_FAIL("Failed to initialize time");

    ret = pci_init();
    if (ret < 0)
        INIT_FAIL("Failed to initialize PCI devices");

    /* switching APIC to x2 mode is not done in vm-common code because e.g. Intel TDX boots already
     * with x2APIC enabled, moreover TDX does not allow to access IA32_APIC_BASE MSR at all */
    ret = switch_apic_to_x2_mode();
    if (ret < 0)
        INIT_FAIL("Failed to switch APIC to x2APIC mode");

    ret = apic_init();
    if (ret < 0)
        INIT_FAIL("Failed to initialize APIC");

    ret = xsave_init();
    if (ret < 0)
        INIT_FAIL("Failed to initialize XSAVE functionality");

    ret = syscalls_init();
    if (ret < 0)
        INIT_FAIL("Failed to initialize system call handling");

    /* must be called before interrupts_init() because it allocates interrupt stacks/XSAVE areas */
    ret = init_multicore_prepare(num_cpus);
    if (ret < 0)
        INIT_FAIL("Failed to initialize multicore preparation (per-CPU data and stacks)");
#ifdef PAL_HOST_SEV
    ret = sev_percpu_prepare(num_cpus);
    if (ret < 0)
        INIT_FAIL("SEV: failed to prepare the per-CPU state (#VC stacks, GHCBs)");
#endif

    /* interrupts must be enabled (via `sti`) after all other parts of the kernel are initialized */
    ret = interrupts_init();
    if (ret < 0)
        INIT_FAIL("Failed to initialize interrupt/exception handling");

    ret = init_multicore(num_cpus, /*hob_list_addr=*/NULL);
    if (ret < 0)
        INIT_FAIL("Failed to initialize multicore (BSP CPU couldn't init AP CPUs)");
    boot_stamp("multicore up");

    if (!g_console)
        INIT_FAIL("Failed to initialize virtio-console driver");
    if (!g_fs)
        INIT_FAIL("Failed to initialize virtio-fs driver");

    ret = memory_tighten_permissions();
    if (ret < 0)
        INIT_FAIL("Failed to tighten memory permissions after initialization");

    /* read VMM (untrusted) inputs: gramine args, environment variables and host's PWD from fw_cfg
     * QEMU pseudo-device, see also kernel_vmm_inputs.h */
    ret = cmdline_init_args(g_cmdline, sizeof(g_cmdline));
    if (ret < 0)
        INIT_FAIL("Can't read command line from VMM");

    ret = cmdline_init_envs(g_envs, sizeof(g_envs));
    if (ret < 0)
        INIT_FAIL("Can't read envinronment variables from VMM");

    ret = host_pwd_init();
    if (ret < 0)
        INIT_FAIL("Can't read host's PWD from VMM");

    ret = virtio_fs_fuse_init();
    if (ret < 0)
        INIT_FAIL("Failed FUSE_INIT request of virtio-fs driver");

    ret = _PalThreadCreate(&g_first_thread_handle, pal_start_continue, g_cmdline);
    if (ret < 0)
        INIT_FAIL("Failed to create first thread");

    _PalThreadYieldExecution();  /* we leave this initial stack forever */
    __builtin_unreachable();
}

noreturn int pal_start_continue(void* cmdline_) {
    int ret;

    const char* cmdline = (const char*)cmdline_;

    /* relocate PAL */
    set_pal_binary_name("pal");
    ret = setup_pal_binary(/*apply_relocations=*/true);
    if (ret < 0)
        INIT_FAIL("Relocation of the PAL binary failed");

    /* allocate as global variables in case we overwrite the current stack */
    static int argc = 0;
    static const char* argv[MAX_ARGV_CNT] = { NULL };

    ret = cmdline_read_gramine_args(cmdline, &argc, &argv[0]);
    if (ret < 0)
        INIT_FAIL("Failed to read Gramine arguments");

    static int envp_cnt = 0;
    ret = cmdline_read_gramine_envs(g_envs, &envp_cnt, &g_vm_envp[0]);
    if (ret < 0)
        INIT_FAIL("Failed to read host environment variables");

    if (argc < 2)
        print_usage_and_exit();

    bool first_process = !strcmp(argv[0], "init");
    if (!first_process)
        INIT_FAIL("Gramine-VM currently runs only in single-process mode (with `init`)");

    char* manifest_path = alloc_concat(argv[1], -1, ".manifest", -1);
    if (!manifest_path)
        INIT_FAIL("Out of memory");

    char* manifest = NULL;
    ret = read_text_file_to_cstr(manifest_path, &manifest, /*out_size=*/NULL);
    if (ret < 0)
        INIT_FAIL("Reading manifest failed");

    g_pal_common_state.raw_manifest_data = manifest;

    char errbuf[256];
    g_pal_public_state.manifest_root = toml_parse(manifest, errbuf, sizeof(errbuf));
    if (!g_pal_public_state.manifest_root)
        INIT_FAIL_MANIFEST(errbuf);

#ifdef __x86_64__
    bool enable_multiprocess;
    ret = toml_bool_in(g_pal_public_state.manifest_root, "sys.experimental__multiprocess",
                       /*defaultval=*/false, &enable_multiprocess);
    if (ret < 0)
        INIT_FAIL("Cannot parse 'sys.experimental__multiprocess'");
    if (enable_multiprocess) {
        ret = init_multiprocess_frame_pool();
        if (ret < 0)
            INIT_FAIL("Cannot initialize the multiprocess frame pool: %s", pal_strerror(ret));
        /* writable shared file mappings come from the PAL's per-file cache (pal_common_files.c) */
        g_pal_public_state.file_maps_write_back = true;
    }
#endif

    ret = toml_bool_in(g_pal_public_state.manifest_root,
                       "sys.enable_extra_runtime_domain_names_conf", /*defaultval=*/false,
                       &g_pal_public_state.extra_runtime_domain_names_conf);
    if (ret < 0)
        INIT_FAIL("Cannot parse 'sys.enable_extra_runtime_domain_names_conf'");

    bool shared_input_ring;
    ret = toml_bool_in(g_pal_public_state.manifest_root,
                       "sys.experimental__shared_input_ring", /*defaultval=*/false,
                       &shared_input_ring);
    if (ret < 0)
        INIT_FAIL("Cannot parse 'sys.experimental__shared_input_ring'");
    virtio_vsock_set_shared_input_ring(shared_input_ring);

    g_pal_public_state.first_thread = g_first_thread_handle;

    g_pal_public_state.attestation_type = "none";

    extern void sysret_asm(void);
    g_pal_public_state.vm_user_return_stub = (void*)&sysret_asm;
    g_pal_public_state.vm_user_csgsfs = 0x33ULL | (0x2bULL << 48); /* user CS 0x33, SS 0x2b (Linux numbering) */
    g_pal_public_state.vm_user_rip_offset = offsetof(struct pal_tcb_vm,
                                                     kernel_thread.context.user_rip);

    ret = pal_common_get_topo_info(&g_pal_public_state.topo_info);
    if (ret < 0)
        INIT_FAIL("Failed to get topology information: %s", pal_strerror(ret));

#ifdef __x86_64__
    if (enable_multiprocess) {
        ret = memory_address_space_clone(g_pml4_table_base, &g_initial_process_address_space);
        if (ret < 0)
            INIT_FAIL("Failed to create the initial process address space: %s", pal_strerror(ret));
        /* Every process is demand-paged from the frame pool, the first one too. With the graphics
         * redirection configured the arenas and the comm page keep their fixed guest-physical
         * ranges and map into any process at those addresses (memory_address_space_alloc() shares
         * the boot root's entries there); the addresses the LibOS hands the host (ioctl arguments
         * and the pointers inside them) are translated by the LibOS, no longer by an identity
         * mapping of the whole process (Documentation/devel/proton-vm/design-gpu.md, option B). */
        g_initial_process_address_space.identity_memory = false;
        struct pal_tcb_vm* current_tcb = (struct pal_tcb_vm*)pal_get_tcb();
        if (!current_tcb)
            INIT_FAIL("Cannot find the initial thread control block");
        current_tcb->kernel_thread.address_space = &g_initial_process_address_space;
        ret = memory_address_space_switch(&g_initial_process_address_space);
        if (ret < 0)
            INIT_FAIL("Failed to switch to the initial process address space: %s", pal_strerror(ret));

        {
            /* Every process's memory is demand-paged from the frame pool through its own page
             * tables, so the application's virtual range need not end with the guest RAM: it
             * reaches the page tables' limit. The kernel's own uses of the RAM end (the frame
             * pool, the arenas, the shared range, the free-memory walk) took their values above;
             * the LibOS places everything beyond RAM lazily. Wine reserves and probes tens of
             * GiB (Documentation/devel/proton-vm/PLAN.md, blocker 6). With graphics the arenas
             * are reserved physical ranges inside the RAM part (the LibOS refuses fixed mappings
             * there), which does not shorten the range. */
            g_pal_public_state.memory_address_end = (void*)MEMORY_ADDRESS_SPACE_LIMIT;
            log_always("Application address space: up to %p (demand-paged processes; guest RAM "
                       "ends at %p%s)", g_pal_public_state.memory_address_end,
                       g_vm_physical_memory_end, g_graphics_configured ? "; graphics arenas" : "");
        }
    }
#endif

    pal_main(/*instance_id=*/0, /*parent_process=*/NULL, g_first_thread_handle, argv + 1, g_vm_envp,
             /*post_callback=*/NULL);
    __builtin_unreachable();
}

/* the child's PAL is initialized; what it allocated until here is freed at its exit */
static void vm_pal_process_started(void) {
    pal_startup_allocs_record(false);
}

noreturn void vm_pal_process_start(PAL_HANDLE parent_process, const char** arguments) {
    struct pal_tcb_vm* tcb = (struct pal_tcb_vm*)pal_get_tcb();
    if (!tcb || !tcb->thread_handle)
        INIT_FAIL("Child process has no PAL thread handle");

    /* The copied process-data segment inherited the parent's LibOS memory callbacks. The child
     * must install its own callbacks during LibOS initialization. */
    PalResetMemoryBookkeepingUpcallsForChild();

    /* All processes of one Gramine instance share its instance id (it names the IPC pipes). The
     * copied process-data segment still holds the parent's public state at this point. */
    uint64_t instance_id = g_pal_public_state.instance_id;
    pal_startup_allocs_record(true);
    pal_main(instance_id, parent_process, tcb->thread_handle, arguments, g_vm_envp,
             vm_pal_process_started);
}
