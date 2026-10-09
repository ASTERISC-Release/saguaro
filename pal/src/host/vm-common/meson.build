# Architecture-neutral sources of the unikernel mini-kernel: device drivers (virtio-pci console,
# fs, vsock), the virtqueue core, futex-time and the PAL-API common layer. These compile for
# both x86_64 and aarch64; anything that touches CPU/arch registers or the platform's firmware
# interface (IDT/GDT vs VBAR, port-I/O fw_cfg vs MMIO fw_cfg, e820 vs DTB) lives in the
# per-architecture lists below (x86_64: same directory, aarch64: arch/aarch64/).
pal_vm_common_sources = files(
    'kernel_files.c',
    'kernel_time.c',
    'kernel_virtio.c',
    'kernel_virtio_console.c',
    'kernel_virtio_fs.c',
    'kernel_virtio_vsock.c',

    'pal_common_console.c',
    'pal_common_eventfd.c',
    'pal_common_events.c',
    'pal_common_files.c',
    'pal_common_object.c',
    'pal_common_pipes.c',
    'pal_common_sockets.c',
    'pal_common_tf.c',

    'external/realpath.c',
)

if host_machine.cpu_family() == 'x86_64'
    # x86_64 arch half: kept in place (not moved into an arch/ subdir) so the x86 build that
    # is in production use stays byte-identical; see the aarch64 counterparts in arch/aarch64/.
    pal_vm_common_sources += files(
        'kernel_events.S',
        'kernel_static_structs.S',

        'kernel_acpi_madt.c',
        'kernel_apic.c',
        'kernel_debug.c',
        'kernel_interrupts.c',
        'kernel_memory.c',
        'kernel_multicore.c',
        'kernel_pci.c',
        'kernel_sched.c',
        'kernel_syscalls.c',
        'kernel_thread.c',
        'kernel_vmm_inputs.c',
        'kernel_xsave.c',

        'pal_common_exception.c',
        'pal_common_misc.c',
        'pal_common_threading.c',
    )
else
    # aarch64: same API, sibling implementations. Quoted includes resolve against the including
    # file's own directory first, so these pick up arch/aarch64/kernel_*.h over the x86 headers
    # where the struct layouts or constants differ (kernel_thread.h/kernel_interrupts.h themselves
    # stay shared via #ifdef because the shared drivers above include them by path).
    pal_vm_common_sources += files(
        'arch/aarch64/kernel_apic.c',
        'arch/aarch64/kernel_context.S',
        'arch/aarch64/kernel_debug.c',
        'arch/aarch64/kernel_dtb.c',
        'arch/aarch64/kernel_fpsimd.c',
        'arch/aarch64/kernel_interrupts.c',
        'arch/aarch64/kernel_memory.c',
        'arch/aarch64/kernel_multicore.c',
        'arch/aarch64/kernel_pci.c',
        'arch/aarch64/kernel_sched.c',
        'arch/aarch64/kernel_syscalls.c',
        'arch/aarch64/kernel_thread.c',
        'arch/aarch64/kernel_vmm_inputs.c',

        'arch/aarch64/pal_common_exception.c',
        'arch/aarch64/pal_common_misc.c',
        'arch/aarch64/pal_common_threading.c',
        'arch/aarch64/pal_vm_tcb.c',
    )
endif

# aarch64 secondary-CPU bring-up uses the PSCI CPU_ON SMC call on the C side (no INIT-SIPI asm
# at all), so the per-arch multicore asm is x86-only; the TDX-only variant stays x86/TDX too.
pal_vm_common_nontdx_sources = files()
pal_vm_common_tdx_sources = files()
if host_machine.cpu_family() == 'x86_64'
    pal_vm_common_nontdx_sources = files(
        'kernel_multicore.S',
    )

    pal_vm_common_tdx_sources = files(
        'kernel_multicore64.S',
    )
endif

pal_vm_common_inc = [
    includes_pal_common,
    # NOTE: for aarch64 the arch dir comes BEFORE '.' — quoted includes from the *shared*
    # sources still resolve against their own (vm-common) directory first and are unaffected;
    # the aarch64-only sources (in vm/ and here, all living elsewhere) then pick the arch
    # headers (kernel_apic.h = GIC, kernel_dtb.h = DTB access) over the x86 ones.
    host_machine.cpu_family() == 'x86_64' ? [] : include_directories('arch/aarch64'),
    include_directories(
        '.',
        '../vm',
        '../../../include/arch/@0@/vm'.format(host_machine.cpu_family()),
        '../../../include/host/linux-common', # virtio-fs driver uses Linux flags
    ),
]

if host_machine.cpu_family() == 'x86_64'
    pal_vm_common_generated_offsets_src = 'generated_offsets.c'
else
    pal_vm_common_generated_offsets_src = 'arch/aarch64/generated_offsets.c'
endif

pal_vm_common_generated_offsets_print = executable('pal-vm-generated-generated-offsets-print',
    pal_vm_common_generated_offsets_src,
    generated_offsets_print_src,
    include_directories: pal_vm_common_inc,
    c_args: cflags_pal_common,
)

pal_vm_common_asm_offsets_h = custom_target('asm-offsets.h[pal-vm]',
    command: [pal_vm_common_generated_offsets_print, '--h'],
    capture: true,
    output: 'asm-offsets.h',
)
