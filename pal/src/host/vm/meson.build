# The BIOS is the x86 boot stage (16-bit trampoline that fw_cfg-DMA-loads the `pal` kernel via
# QEMU's "-device loader"). QEMU virt loads the `pal` ELF directly with "-kernel" and enters
# _start in arch/aarch64/vm_bootloader.S, so there is nothing to build here on aarch64.
if host_machine.cpu_family() == 'x86_64'
    pal_vm_bios_lds = meson.current_source_dir() / 'bios' / 'bios.lds'
    pal_vm_bios_src = meson.current_source_dir() / 'bios' / 'bios.S'

    libpal_vm_bios = executable('bios',
        pal_vm_bios_src,

        link_args: [
            '-nostdlib',
            '-static',
            '-Wl,-T@0@'.format(pal_vm_bios_lds),
        ],
        link_depends: pal_vm_bios_lds,

        install: true,
        install_dir: pkglibdir / 'vm',
    )
endif

if host_machine.cpu_family() == 'x86_64'
    pal_vm_lds = meson.current_source_dir() / 'pal.lds'
else
    pal_vm_lds = meson.current_source_dir() / 'arch/aarch64/pal.lds'
endif

pal_vm_map = custom_target('pal.map[vm]',
    command: [gen_pal_map_prog, '@INPUT@', '@OUTPUT@'],
    input: ['../../pal_symbols', 'pal.map.template'],
    output: 'pal.map',
)

pal_vm_sources = files(
    'pal_console.c',
    'pal_devices.c',
    'pal_eventfd.c',
    'pal_events.c',
    'pal_exception.c',
    'pal_files.c',
    'pal_memory.c',
    'pal_misc.c',
    'pal_object.c',
    'pal_pipes.c',
    'pal_rtld.c',
    'pal_sockets.c',
    'pal_streams.c',
    'pal_threading.c',
)

if host_machine.cpu_family() == 'x86_64'
    pal_vm_sources += files(
        'pal_main.c',
        'pal_process.c',
        'vm_bootloader.S',
        'vm_callbacks.c',
    )
else
    # aarch64: boot sequence (DTB entry), ELF entry trampoline (no BIOS stage), and the
    # port-io/shared-memory callbacks as plain MMIO; single-process pal_process.c (the
    # multiprocess support is x86_64 only).
    pal_vm_sources += files(
        'arch/aarch64/pal_main.c',
        'arch/aarch64/pal_process.c',
        'arch/aarch64/vm_bootloader.S',
        'arch/aarch64/vm_callbacks.c',
    )
endif

libpal_vm = executable('pal',
    pal_vm_sources,
    pal_vm_common_sources,
    pal_vm_common_nontdx_sources,
    pal_vm_common_asm_offsets_h,
    pal_common_sources,

    include_directories: [
        includes_pal_common,
        pal_vm_common_inc,
        include_directories('.'),
    ],

    c_args: [
        cflags_pal_common,
        '-DHOST_TYPE=VM',
        '-DPAL_HOST_VM=1',

        # instruct GCC not to use the AMD64 ABI 128B redzone which resides below the stack
        # pointer, or kernel will be interrupt unsafe
        # (https://wiki.osdev.org/Creating_a_64-bit_kernel). Aarch64 has no redzone, so the
        # flag does not exist there.
        host_machine.cpu_family() == 'x86_64' ? ['-mno-red-zone'] : [],

        # AArch64: the kernel must never execute FPSIMD instructions. Beyond -O3
        # auto-vectorizing our plain-C byte loops (memcpy/memmove/memset are C), even the
        # *movmem expansion* of ordinary struct copies emits `ldp q0, q1` / `str q` from
        # addresses GCC cannot prove 16-byte aligned -- and AArch64 *always* faults unaligned
        # accesses of >= 16 bytes (there is no enable bit to turn this off; under direct-PAL
        # the host Linux kernel's alignment-fixup slow path silently rescues such accesses,
        # inside the unikernel nothing does; both were observed in B1 bring-up: _real_memcpy
        # `str q0` and buf_printf's `ldp q` copy of the variadic frame). The kernel has no FP
        # context of its own (app FP state is saved in the switch asm only), so build the
        # whole kernel general-regs-only. Note: hand-written .S is unaffected; the separately
        # built mbedtls static lib is unaffected too (it is dead code in the vm flavor).
        host_machine.cpu_family() == 'aarch64' ? ['-mgeneral-regs-only'] : [],

        '-Wno-address-of-packed-member',

        cflags_custom_stack_protector,
        cflags_sanitizers,
    ],

    link_args: [
        '-nostdlib',

        # For PAL symbol resolution, we use old-style hash (DT_HASH) table instead of the
        # default GNU hash (DT_GNU_HASH) table -- to figure out the number of dynamic symbols in
        # the SYMTAB symbol table
        '-Wl,--hash-style=sysv',

        # Generic PAL binary verification in pal_rtld.c requires DT_SONAME
        '-Wl,-soname=libpal.so',

        # PAL is expected to be linked with RELRO (Relocation Read-Only)
        '-Wl,-zrelro',
        '-Wl,-znow',

        '-Wl,-zcombreloc',
        '-Wl,-zdefs',
        '-Wl,--version-script=@0@'.format(pal_vm_map.full_path()),
        '-Wl,-T@0@'.format(pal_vm_lds),
        '-Wl,--eh-frame-hdr',
    ],

    pie: true,

    # by default, compilers don't export symbols for executables, revert this behavior
    export_dynamic: true,

    link_depends: [
        pal_vm_map,
        pal_vm_lds,
    ],

    dependencies: [
        common_dep,
        cryptoadapter_dep,
    ],

    install: true,
    install_dir: pkglibdir / 'vm',
)

libpal_vm_dep = declare_dependency(
    link_with: libpal_vm,
)

if debug
    custom_target('libpal.so.map',
        input: libpal_vm,
        command: gen_symbol_map_cmd,
        capture: true,
        output: 'libpal.so.map',

        install: true,
        install_dir: pkglibdir / 'vm',
    )
endif
