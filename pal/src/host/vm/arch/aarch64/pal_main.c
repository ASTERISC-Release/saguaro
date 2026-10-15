/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2023 Intel Corporation */

/*
 * aarch64 counterpart of vm/pal_main.c: kernel bootstrap and manifest/args processing.
 *
 * Boot differences from x86_64 (see arch/aarch64/vm_bootloader.S for the asm entry):
 *   - QEMU virt loads this ELF with `-kernel` and enters EL1 at `_start` with x0 = DTB pointer;
 *     there is no separate BIOS binary and no fw_cfg-DMA loading of the kernel itself (x86's
 *     bios.S + "-device loader" dance disappears).
 *   - The memory map comes from the DTB, not e820 fw_cfg (a synthetic one-entry e820 array is
 *     built so the shared memory_init()/memory_preload_ranges() prototypes stay identical).
 *   - The generic timer frequency replaces the invariant-TSC CPUID probing.
 *   - GIC replaces the local APIC (no x2APIC switching step: GIC has no mode to switch to).
 * Everything from the manifest/args processing half of pal_main.c onwards is copied verbatim,
 * because from the PAL API level up the two architectures must behave identically.
 */

#include <stdatomic.h>
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
#include "kernel_dtb.h"
#include "kernel_files.h"
#include "kernel_interrupts.h"
#include "kernel_memory.h"
#include "kernel_multicore.h"
#include "kernel_pci.h"
#include "kernel_sched.h"
#include "kernel_syscalls.h"
#include "kernel_time.h"
#include "kernel_virtio.h"
#include "kernel_vmm_inputs.h"
#include "kernel_xsave.h"

uint64_t g_tsc_mhz;

static struct pal_handle* g_first_thread_handle = NULL;
/* the host environment, also given to every child process (vm_pal_process_start()) */
static const char* g_vm_envp[MAX_ENVS_CNT];

/*
 * Multiprocess support (`sys.experimental__multiprocess`, Documentation/devel/vm-multiproc-aarch64/):
 * the frame pool, the first process's own root and the boot self-checks, as in the x86_64
 * pal_main.c. The checks prove the address-space invariants on every boot with the key: two roots
 * map one address to different frames, private PAL process data, a root cloned from a process
 * root survives its parent, a scheduled thread runs in its own root, and every frame returns to
 * the pool.
 */
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

static int run_thread_in_space(struct memory_address_space* space,
                               struct address_space_thread_check* check) {
    atomic_init(&check->done, false);
    PAL_HANDLE thread;
    int ret = pal_common_thread_create_with_address_space(&thread, address_space_thread_check,
                                                          check, space);
    if (ret < 0)
        return ret;
    while (!atomic_load_explicit(&check->done, memory_order_acquire))
        _PalThreadYieldExecution();
    while (!sched_thread_is_stopped(thread->thread.kernel_thread))
        _PalThreadYieldExecution();
    PalObjectDestroy(thread);
    return 0;
}

#define SELFCHECK(cond, ...)                 \
    do {                                     \
        if (!(cond)) {                       \
            log_error(__VA_ARGS__);          \
            ret = ret < 0 ? ret : -PAL_ERROR_DENIED; \
            goto out;                        \
        }                                    \
    } while (0)

/* The checks of the x86_64 boot (pal_main.c, init_multiprocess_frame_pool_sized()), plus the
 * TTBR1 alias and a measurement of the address-space switch. */
static int address_space_selfcheck(void) {
    int ret = vm_process_stream_selfcheck();
    if (ret < 0) {
        log_error("Process stream self-check failed: %s", pal_strerror(ret));
        return ret;
    }
    log_always("Process stream I/O, endpoint transfer, shutdown, and frame-reclaim self-check "
               "passed");

    /* PAL thread stacks are recycled instead of freed: warm one before the reclaim baseline */
    struct address_space_thread_check warmup = {.addr = g_address_space_selfcheck_page};
    ret = run_thread_in_space(memory_address_space_boot(), &warmup);
    if (ret < 0)
        return ret;
    bool warmup_ok = warmup.observed == 0 && *g_address_space_selfcheck_page == 0x7766554433221100UL;
    *g_address_space_selfcheck_page = 0;
    if (!warmup_ok)
        return -PAL_ERROR_DENIED;

    ret = vm_process_runtime_copy_selfcheck();
    if (ret < 0) {
        log_error("PAL root state self-check failed: %s", pal_strerror(ret));
        return ret;
    }

    size_t initial_frames = memory_frame_allocated_count();
    uintptr_t frame1 = 0, frame2 = 0, reused = 0;
    bool frame2_owned = false, reused_owned = false;
    struct memory_address_space child = {0};
    struct memory_address_space grandchild = {0};
    bool child_live = false, grandchild_live = false;
    uintptr_t test_addr = (uintptr_t)g_address_space_selfcheck_page;
    volatile uint64_t* parent_value = (volatile uint64_t*)test_addr;
    uint64_t original_value = *parent_value;
    uint64_t original_pdata = g_process_data_selfcheck;
    const uint64_t parent_marker = 0x1122334455667788UL;
    const uint64_t child_marker = 0x8877665544332211UL;
    const uint64_t grandchild_marker = 0x1029384756abcdefUL;

    /* frames: distinct, aligned, double free rejected, reuse zeroed; the TTBR1 alias reaches the
     * same memory as the identity address (which the boot root maps EL1-only) */
    ret = memory_frame_alloc(&frame1);
    SELFCHECK(ret == 0, "frame-pool self-check allocation 1 failed: %s", pal_strerror(ret));
    ret = memory_frame_alloc(&frame2);
    SELFCHECK(ret == 0, "frame-pool self-check allocation 2 failed: %s", pal_strerror(ret));
    SELFCHECK(frame1 != frame2 && IS_ALIGNED(frame1, PAGE_SIZE) && IS_ALIGNED(frame2, PAGE_SIZE),
              "frame-pool self-check: bad frames %lx %lx", frame1, frame2);
    SELFCHECK(memory_frame_pointer(frame1) != (void*)frame1 &&
              memory_frame_is_alias_pointer(memory_frame_pointer(frame1)),
              "frame alias self-check: frame %lx has no TTBR1 alias", frame1);
    *(volatile uint64_t*)memory_frame_pointer(frame1) = 0xfeedfacecafebeefUL;
    SELFCHECK(*(volatile uint64_t*)frame1 == 0xfeedfacecafebeefUL,
              "frame alias self-check: alias and identity differ at %lx", frame1);
    ret = memory_frame_free(frame1);
    SELFCHECK(ret == 0, "frame-pool self-check free failed: %s", pal_strerror(ret));
    ret = memory_frame_free(frame1);
    SELFCHECK(ret == -PAL_ERROR_INVAL, "frame-pool self-check double-free returned %s",
              pal_strerror(ret));
    ret = memory_frame_alloc(&reused);
    SELFCHECK(ret == 0 && reused == frame1 && *(uint64_t*)memory_frame_pointer(reused) == 0,
              "frame-pool self-check: frame %lx not reused zeroed (%lx)", frame1, reused);
    frame1 = 0;
    log_always("Frame-pool allocate, double-free rejection, frame-reuse and TTBR1 alias "
               "self-check passed");

    *parent_value = parent_marker;
    g_process_data_selfcheck = parent_marker;

    /* a lazy copy of the boot root with private process data and its own frame at test_addr */
    ret = memory_address_space_clone(g_pml4_table_base, &child);
    child_live = ret == 0;
    if (ret == 0)
        ret = memory_address_space_copy_private_range(
                memory_address_space_boot(), &child, (uintptr_t)__process_data_start,
                (uintptr_t)__process_data_end - (uintptr_t)__process_data_start);
    if (ret == 0) {
        ret = memory_address_space_map_owned(&child, test_addr, reused, /*write=*/true,
                                             /*execute=*/false, /*usermode=*/true);
        reused_owned = ret == 0;
    }
    SELFCHECK(ret == 0, "Address-space child clone/map self-check failed: %s", pal_strerror(ret));

    ret = memory_address_space_switch(&child);
    SELFCHECK(ret == 0, "Address-space child switch self-check failed: %s", pal_strerror(ret));
    uint64_t child_initial = *(volatile uint64_t*)test_addr;
    *(volatile uint64_t*)test_addr = child_marker;
    bool child_pdata_initial = g_process_data_selfcheck == parent_marker;
    g_process_data_selfcheck = child_marker;

    /* a full copy of the child (as a fork) with its own frame at test_addr */
    ret = memory_address_space_clone_from(&child, &grandchild);
    grandchild_live = ret == 0;
    if (ret == 0)
        ret = memory_address_space_copy_private_range(
                &child, &grandchild, (uintptr_t)__process_data_start,
                (uintptr_t)__process_data_end - (uintptr_t)__process_data_start);
    if (ret == 0) {
        ret = memory_address_space_map_owned(&grandchild, test_addr, frame2, /*write=*/true,
                                             /*execute=*/false, /*usermode=*/true);
        frame2_owned = ret == 0;
    }
    if (ret < 0)
        memory_address_space_switch_root(g_pml4_table_base);
    SELFCHECK(ret == 0, "Address-space grandchild clone/map self-check failed: %s",
              pal_strerror(ret));

    ret = memory_address_space_switch(&grandchild);
    if (ret < 0)
        memory_address_space_switch_root(g_pml4_table_base);
    SELFCHECK(ret == 0, "Address-space grandchild switch self-check failed: %s",
              pal_strerror(ret));
    bool grandchild_isolated = *(volatile uint64_t*)test_addr == 0 &&
                               g_process_data_selfcheck == child_marker;
    *(volatile uint64_t*)test_addr = grandchild_marker;
    g_process_data_selfcheck = grandchild_marker;

    ret = memory_address_space_switch_root(g_pml4_table_base);
    SELFCHECK(ret == 0, "Address-space boot switch self-check failed: %s", pal_strerror(ret));
    bool isolated = child_initial == 0 && child_pdata_initial && grandchild_isolated &&
                    g_process_data_selfcheck == parent_marker && *parent_value == parent_marker &&
                    *(volatile uint64_t*)memory_frame_pointer(reused) == child_marker &&
                    *(volatile uint64_t*)memory_frame_pointer(frame2) == grandchild_marker;
    SELFCHECK(isolated, "Address-space isolation failed: child=%lx pdata=%d grandchild=%d "
              "parent=%lx child-frame=%lx grandchild-frame=%lx", child_initial,
              child_pdata_initial, grandchild_isolated, *parent_value,
              *(volatile uint64_t*)memory_frame_pointer(reused),
              *(volatile uint64_t*)memory_frame_pointer(frame2));

    /* the cost of an address-space switch (TTBR0 write and a full local TLB invalidation; no
     * ASIDs), measured on this vCPU: decision 4 of the plan */
    uint64_t t0 = get_tsc();
    for (int i = 0; i < 1000; i++) {
        memory_address_space_switch(&child);
        (void)*(volatile uint64_t*)test_addr;
        memory_address_space_switch_root(g_pml4_table_base);
        (void)*(volatile uint64_t*)test_addr;
    }
    uint64_t t1 = get_tsc();
    log_always("Address-space switch: %lu ns per switch (TTBR0 + full local TLB invalidation, "
               "2000 switches)", (unsigned long)((t1 - t0) * 1000 / g_tsc_mhz / 2000));

    /* a descendant owns a full private table tree and must survive its parent's teardown */
    memory_address_space_destroy(&child);
    child_live = false;
    reused_owned = false; /* released with the child */
    reused = 0;
    ret = memory_address_space_switch(&grandchild);
    if (ret < 0)
        memory_address_space_switch_root(g_pml4_table_base);
    SELFCHECK(ret == 0, "Address-space grandchild resume self-check failed: %s",
              pal_strerror(ret));
    bool survived = *(volatile uint64_t*)test_addr == grandchild_marker &&
                    g_process_data_selfcheck == grandchild_marker;
    ret = memory_address_space_switch_root(g_pml4_table_base);
    SELFCHECK(ret == 0 && survived, "Address-space descendant did not survive its parent");

    /* a thread scheduled in the grandchild's root sees the grandchild's frame */
    struct address_space_thread_check check = {.addr = (volatile uint64_t*)test_addr};
    ret = run_thread_in_space(&grandchild, &check);
    SELFCHECK(ret == 0, "Address-space scheduler-thread create self-check failed: %s",
              pal_strerror(ret));
    bool scheduled = check.observed == grandchild_marker && *parent_value == parent_marker &&
                     *(volatile uint64_t*)memory_frame_pointer(frame2) == 0x7766554433221100UL;
    SELFCHECK(scheduled, "Scheduler root check values: observed=%lx parent=%lx frame=%lx",
              check.observed, *parent_value, *(volatile uint64_t*)memory_frame_pointer(frame2));

    memory_address_space_destroy(&grandchild);
    grandchild_live = false;
    frame2_owned = false;
    frame2 = 0;
    SELFCHECK(memory_frame_allocated_count() == initial_frames,
              "Address-space frame leak: started %zu, ended %zu", initial_frames,
              memory_frame_allocated_count());
    log_always("Address-space isolation, nested clone, parent teardown, frame reclaim, and "
               "scheduler self-check passed");
    ret = 0;

out:
    *parent_value = original_value;
    g_process_data_selfcheck = original_pdata;
    if (grandchild_live)
        memory_address_space_destroy(&grandchild);
    if (child_live)
        memory_address_space_destroy(&child);
    if (frame1)
        memory_frame_free(frame1);
    if (frame2 && !frame2_owned)
        memory_frame_free(frame2);
    if (reused && !reused_owned)
        memory_frame_free(reused);
    return ret;
}

/* The pool takes half of the largest free stretch of guest RAM (2 MB aligned, at most 4 GB): its
 * identity addresses are reserved from application use, and on AArch64 the PAL's early
 * allocations sit at the top of RAM, so there is no 1-GB-aligned stretch to take as on x86_64.
 * Every process's memory (the LibOS included) and, after the switch below, the PAL heap come
 * from the pool; the other half stays application address space. */
static int init_multiprocess_frame_pool(void) {
    extern struct pal_initial_mem_range g_initial_mem_ranges[];
    const uintptr_t align = 2UL * 1024 * 1024;
    uintptr_t lo = ALIGN_UP((uintptr_t)g_pal_public_state.memory_address_start, align);
    uintptr_t hi = ALIGN_DOWN((uintptr_t)g_pal_public_state.memory_address_end, align);

    /* the ranges are sorted from high to low addresses */
    uintptr_t best_start = 0, best_size = 0;
    uintptr_t gap_end = hi;
    for (size_t i = 0; i <= g_pal_public_state.initial_mem_ranges_len; i++) {
        uintptr_t gap_start = lo;
        uintptr_t next_end = 0;
        if (i < g_pal_public_state.initial_mem_ranges_len) {
            gap_start = MAX(lo, ALIGN_UP(g_initial_mem_ranges[i].end, align));
            next_end = ALIGN_DOWN(g_initial_mem_ranges[i].start, align);
        }
        if (gap_end > gap_start && gap_end - gap_start > best_size) {
            best_start = gap_start;
            best_size = gap_end - gap_start;
        }
        if (i < g_pal_public_state.initial_mem_ranges_len)
            gap_end = MIN(gap_end, next_end);
        if (gap_end <= lo)
            break;
    }
    size_t pool_size = MIN(4UL * 1024 * 1024 * 1024, ALIGN_DOWN(best_size / 2, align));
    if (pool_size < 64UL * 1024 * 1024)
        return -PAL_ERROR_NOMEM;

    int ret = pal_add_initial_range(best_start, pool_size, /*prot=*/0, /*is_user_reserved=*/false,
                                    "private_frame_pool");
    if (ret < 0)
        return ret;
    ret = memory_frame_pool_init(best_start, pool_size);
    if (ret < 0)
        return ret;
    ret = pal_enable_shared_frame_slab_allocator();
    if (ret < 0)
        return ret;
    return address_space_selfcheck();
}

/* ticks per microsecond, the role g_tsc_mhz plays on x86 (kernel_time.c does
 * `diff_tsc / g_tsc_mhz`); computed from the generic counter frequency with round-to-nearest.
 * TODO(Milestone-B): QEMU virt's counter runs at 62.5MHz, so rounding to 63 us/tick drifts
 * ~0.8%; kernel_time.c should keep Hz-scale values once aarch64-only. */
static int tick_frequency_init(void) {
    uint64_t cntfrq = dtb_cntfrq();
    if (!cntfrq)
        return -PAL_ERROR_DENIED;
    uint64_t ticks_per_us = (cntfrq + 500000UL) / 1000000UL;
    if (!ticks_per_us) {
        /* counter slower than 1MHz: keep at least 1 to avoid divide-by-zero; time will be
         * coarse but monotonic */
        ticks_per_us = 1;
    }
    g_tsc_mhz = ticks_per_us;
    return 0;
}

static int add_preloaded_range(uintptr_t addr, size_t size, bool is_user_reserved,
                               const char* comment) {
    return pal_add_initial_range(addr, size, /*pal_prot=*/0, is_user_reserved, comment);
}

noreturn static void print_usage_and_exit(void) {
    log_always("USAGE: init <application> args...");
    log_always("This is an internal interface. Use gramine-vm to launch applications.");
    _PalProcessExit(1);
}

noreturn int pal_start_continue(void* cmdline_);

/* called by `arch/aarch64/vm_bootloader.S` on kernel startup, with x0 = DTB physical address */
__attribute_no_stack_protector  /* starts with a boot per-CPU struct, no thread TCB yet */
__attribute_no_sanitize_address /* starts with boot page tables w/o ASan shared memory */
noreturn void pal_start_c(const void* fdt) {
    int ret;

    /* The asm entry already installed `g_early_per_cpu_data` into TPIDR_EL1 (the aarch64
     * stand-in for GS_KERNEL_BASE); every get_per_cpu_data() from here on works. */
    set_dummy_gs_base();

    /* initialize alloc_align as early as possible, a lot of PAL APIs depend on this being set */
    g_pal_public_state.alloc_align = PAGE_SIZE;
    assert(IS_POWER_OF_2(g_pal_public_state.alloc_align));

    /* QEMU virt's direct-boot protocol places the generated DTB at the first DRAM bank base
     * when no initrd is present. On qemu-system-aarch64 10.2 (TCG) the vCPU enters _start
     * with x0 == 0 despite boot.c's bootloader_aarch64 stub loading it there, so fall back
     * to the conventional address; dtb_init() authenticates the FDT magic regardless, so a
     * wrong guess here still produces the INIT_FAIL below rather than parsing garbage. */
    if (!fdt) {
        fdt = (const void*)0x40000000;
    }

    ret = dtb_init(fdt);
    if (ret < 0)
        INIT_FAIL("No valid device tree passed at boot (x0=%p)", fdt);

    /* DTB /cpus drives the CPU count (PSCI CPU_ON ids come from the same nodes, see
     * init_multicore()). The one hardware feature the SMP path cannot do without is the PSCI
     * conduit -- QEMU's TCG PSCI is advertised through /psci, and without it no secondary CPU
     * can ever be started, so fall back to a single-CPU boot (announced, not silent) rather
     * than failing the whole unikernel on a PSCI-less machine. */
    uint32_t num_cpus = dtb_num_cpus();
    if (num_cpus > 1 && !dtb_psci_available()) {
        log_always("device tree describes %u CPUs but no /psci conduit is available; running "
                   "on CPU 0 only", num_cpus);
        num_cpus = 1;
    }

    /* synthesize the e820-shaped array that memory_init()/memory_preload_ranges() consume --
     * same one-entry "system RAM" table the DTB describes (x86 read this from QEMU's "etc/e820"
     * fw_cfg blob, which has no arm equivalent) */
    e820_table_entry e820[E820_NR_ENTRIES] = { {
        .address = g_dtb_mem_start,
        .size    = g_dtb_mem_size,
        .type    = E820_ADDRESS_RANGE_MEMORY,
    } };
    size_t e820_size = sizeof(e820[0]);

    ret = memory_init(e820, e820_size, &g_pal_public_state.memory_address_start,
                                      &g_pal_public_state.memory_address_end);
    if (ret < 0)
        INIT_FAIL("Can't determine memory range from device tree");

    /* vm_bootloader.S installed identity page tables covering [0..4GB) with the QEMU-virt
     * device/normal attribute split; this builds the full VMSA tables (Milestone-B follow-up) */
    ret = memory_pagetables_init(g_pal_public_state.memory_address_end,
                                 /*current_page_tables_cover_1gb=*/false);
    if (ret < 0)
        INIT_FAIL("Failed to initialize page tables");

    ret = memory_preload_ranges(e820, e820_size, &add_preloaded_range);
    if (ret < 0)
        INIT_FAIL("Failed to initialize preloaded ranges");

    /* PAL binary is loaded by QEMU at 0x40080000 and spans through the rx segment end (__text_end:
     * ro + rw/64MB .bss + rx; see arch/aarch64/pal.lds). memory_init() already keeps the
     * allocator floor above it, this obstacle range is the same defensive reservation x86's
     * pal_main.c adds below its floor.
     * FIXME: whole PAL binary is RWX because the boot map is RWX and memory_pagetables_init()
     *        is not implemented yet, so zero/protection passes below are skipped. */
    extern const char __text_end[];
    ret = add_preloaded_range(g_dtb_mem_start + 0x80000UL,
                              ALIGN_UP((uintptr_t)__text_end, PAGE_SIZE)
                                  - (g_dtb_mem_start + 0x80000UL),
                              /*is_user_reserved=*/false, "pal_binary");
    if (ret < 0)
        INIT_FAIL("Failed to preload PAL-binary memory range");

    /*
     * x86 zeroes all free pages here and marks them PROT_NONE (QEMU/KVM does not guarantee
     * freshly-allocated pages are zero). Skipped on aarch64 until memory_pagetables_init()
     * exists: the boot map only covers the low 4GB, so touching pages near the top of an
     * 8GB-size VM would take a stage-2 translation fault, and page_protect is a no-op anyway.
     * The slab manager zeroes pages on (re)allocation in the meantime.
     */

    call_init_array();

    init_slab_mgr();

    /* must be before gic_init() since the latter reads CNTFRQ for the timer period */
    ret = tick_frequency_init();
    if (ret < 0)
        INIT_FAIL("Failed to initialize generic timer frequency");

    ret = time_init();
    if (ret < 0)
        INIT_FAIL("Failed to initialize time");

    ret = pci_init();
    if (ret < 0)
        INIT_FAIL("Failed to initialize PCI devices");

    /* there is no "switch GIC to X2GIC mode" (the GIC has no such modes); GIC distributor and
     * the EL1 timer interrupt line are initialized here, mirroring apic_init() on x86 */
    ret = gic_init();
    if (ret < 0)
        INIT_FAIL("Failed to initialize GIC");

    ret = xsave_init();
    if (ret < 0)
        INIT_FAIL("Failed to initialize FP/SIMD access (CPACR_EL1.FPEN)");

    ret = syscalls_init();
    if (ret < 0)
        INIT_FAIL("Failed to initialize exception vector table (VBAR_EL1)");

    /* must be called before interrupts_init() because it allocates interrupt stacks/FP areas */
    ret = init_multicore_prepare(num_cpus);
    if (ret < 0)
        INIT_FAIL("Failed to initialize multicore preparation (per-CPU data and stacks)");

    /* interrupts must be unmasked (via `sti`) after all other parts of the kernel are ready */
    ret = interrupts_init();
    if (ret < 0)
        INIT_FAIL("Failed to initialize interrupt/exception handling");

    ret = init_multicore(num_cpus);
    if (ret < 0)
        INIT_FAIL("Failed to initialize multicore (BSP CPU couldn't init AP CPUs)");

    if (!g_console)
        INIT_FAIL("Failed to initialize virtio-console driver");
    if (!g_fs)
        INIT_FAIL("Failed to initialize virtio-fs driver");

    ret = memory_tighten_permissions();
    if (ret < 0)
        INIT_FAIL("Failed to tighten memory permissions after initialization");

    /* read VMM (untrusted) inputs: gramine args, environment variables and host's PWD from the
     * fw_cfg MMIO device, see also kernel_vmm_inputs.h */
    ret = cmdline_init_args(g_cmdline, sizeof(g_cmdline));
    if (ret < 0)
        INIT_FAIL("Can't read command line from VMM");

    ret = cmdline_init_envs(g_envs, sizeof(g_envs));
    if (ret < 0)
        INIT_FAIL("Can't read environment variables from VMM");

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

/*
 * From here on, identical to the x86 pal_main.c's second half: PAL self-relocation, Gramine
 * args/envs parsing, manifest loading, and handover to the generic pal_main(). Any divergence
 * between the architectures from this point would be a bug to fix in both copies at once.
 */
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

    bool enable_multiprocess;
    ret = toml_bool_in(g_pal_public_state.manifest_root, "sys.experimental__multiprocess",
                       /*defaultval=*/false, &enable_multiprocess);
    if (ret < 0)
        INIT_FAIL("Cannot parse 'sys.experimental__multiprocess'");
    if (enable_multiprocess) {
        log_always("'sys.experimental__multiprocess' on AArch64 is work in progress "
                   "(Documentation/devel/vm-multiproc-aarch64/)");
        ret = init_multiprocess_frame_pool();
        if (ret < 0)
            INIT_FAIL("Cannot initialize the multiprocess frame pool: %s", pal_strerror(ret));
        /* writable shared file mappings come from the PAL's per-file cache (pal_common_files.c) */
        g_pal_public_state.file_maps_write_back = true;
    }

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

    /* where the asm stores the ELR_EL1 snapshot for the `svc` window (used by exception/signal
     * delivery to find "user RIP" of the syscalling thread) */
    g_pal_public_state.vm_user_rip_offset = offsetof(struct pal_tcb_vm,
                                                     kernel_thread.context.user_rip);
    extern void pal_sigreturn_return(void); /* arch/aarch64/kernel_context.S */
    extern void pal_syscall_return(void);
    g_pal_public_state.vm_signal_return_trampoline = (uintptr_t)&pal_sigreturn_return;
    g_pal_public_state.vm_syscall_return_trampoline = (uintptr_t)&pal_syscall_return;

    ret = pal_common_get_topo_info(&g_pal_public_state.topo_info);
    if (ret < 0)
        INIT_FAIL("Failed to get topology information: %s", pal_strerror(ret));

    if (enable_multiprocess) {
        ret = memory_address_space_clone(g_pml4_table_base, &g_initial_process_address_space);
        if (ret < 0)
            INIT_FAIL("Failed to create the initial process address space: %s", pal_strerror(ret));
        struct pal_tcb_vm* current_tcb = (struct pal_tcb_vm*)pal_get_tcb();
        if (!current_tcb)
            INIT_FAIL("Cannot find the initial thread control block");
        current_tcb->kernel_thread.address_space = &g_initial_process_address_space;
        ret = memory_address_space_switch(&g_initial_process_address_space);
        if (ret < 0)
            INIT_FAIL("Failed to switch to the initial process address space: %s",
                      pal_strerror(ret));
    }

    pal_main(/*instance_id=*/0, /*parent_process=*/NULL, g_first_thread_handle, argv + 1,
             g_vm_envp, /*post_callback=*/NULL);
    __builtin_unreachable();
}

/* the child's PAL is initialized; what it allocated until here is freed at its exit */
static void vm_pal_process_started(void) {
    pal_startup_allocs_record(false);
}

/* First code of a child process, on its first thread in its own root (pal_process.c). */
noreturn void vm_pal_process_start(PAL_HANDLE parent_process, const char** arguments);
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
