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
    static const char* envp[MAX_ENVS_CNT] = { NULL };

    ret = cmdline_read_gramine_envs(g_envs, &envp_cnt, &envp[0]);
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

    pal_main(/*instance_id=*/0, /*parent_process=*/NULL, g_first_thread_handle, argv + 1, envp,
             /*post_callback=*/NULL);
    __builtin_unreachable();
}
