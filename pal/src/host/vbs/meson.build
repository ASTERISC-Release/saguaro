# The Windows VBS-enclave PAL has two parts (dev docs: vbs-enclave/PLAN.md, D1-D3 and the
# design of the hello-world path):
#
#  - the Windows enclave runtime and host (enclave/, host/, common/): Windows ARM64 code, built with
#    clang --target=aarch64-pc-windows-msvc and linked/signed on Windows by scripts/vbs/, not here;
#  - the vbs PAL proper (this directory): an AArch64 Linux ELF libpal.so built here from the common
#    PAL sources, packed into the signed enclave image by scripts/vbs/pack.py and entered by the
#    Windows runtime with a table of its services.
#
# -Dvbs=enabled builds the vbs PAL proper (aarch64 only) and a LibOS built for it
# (-DPAL_HOST_VBS=1, libos/src/meson.build). It must be the only PAL enabled.

if host_machine.cpu_family() not in ['aarch64', 'x86_64']
    error('-Dvbs=enabled: the VBS-enclave PAL targets AArch64 (ARM64 Windows) and x86_64 (x64 Windows)')
endif

if direct or sgx or vm or tdx or skeleton
    error('-Dvbs=enabled must be the only PAL enabled (the LibOS is built for it)')
endif

subdir('arch/@0@'.format(host_machine.cpu_family()))

pal_vbs_map = custom_target('pal.map[vbs]',
    command: [gen_pal_map_prog, '@INPUT@', '@OUTPUT@'],
    input: ['../../pal_symbols', 'pal.map.template'],
    output: 'pal.map',
)

cflags_vbs = [
    '-DHOST_TYPE=VBS',
    '-DPAL_HOST_VBS=1',
    # AArch64: x18 is the enclave TEB of the Windows runtime (PLAN.md D4): no Linux code of ours
    # uses it (x86_64: nothing; PLAN-x64.md)
    cflags_vbs_arch,
]

libpal_vbs = shared_library('pal',
    'pal_console.c',
    'pal_devices.c',
    'pal_eventfd.c',
    'pal_events.c',
    'pal_exception.c',
    'pal_files.c',
    'pal_hostio.c',
    'pal_main.c',
    'pal_memory.c',
    'pal_misc.c',
    'pal_object.c',
    'pal_pipes.c',
    'pal_process.c',
    'pal_rtld.c',
    'pal_sockets.c',
    'pal_streams.c',
    'pal_threading.c',
    pal_vbs_sources_arch,
    pal_common_sources,

    include_directories: [
        includes_pal_common,
        include_directories(
            '../../../include/arch/@0@/vbs'.format(host_machine.cpu_family()),
            'common',
        ),
    ],

    c_args: [
        cflags_pal_common,
        cflags_sanitizers,
        cflags_custom_stack_protector,
        cflags_vbs,
        # the copied skeleton stubs leave their parameters unused
        '-Wno-unused-parameter',
    ],

    link_args: [
        '-nostdlib',
        '-Wl,--hash-style=sysv',
        '-Wl,-zrelro',
        '-Wl,-znow',
        '-Wl,-zcombreloc',
        '-Wl,-zdefs',
        # strict W^X per page in the packed image: text and data on different 64 KiB pages
        '-Wl,-zmax-page-size=0x10000',
        '-Wl,--version-script=@0@'.format(pal_vbs_map.full_path()),
        '-Wl,-T@0@'.format(pal_vbs_lds),
        '-Wl,--eh-frame-hdr',
    ],
    link_depends: [
        pal_vbs_map,
        pal_vbs_lds,
    ],

    dependencies: [
        common_dep,
        cryptoadapter_dep, # SHA-256 of trusted host files (mbedtls, with -ffixed-x18 for vbs)
    ],

    install: true,
    install_dir: pkglibdir / 'vbs',
)

libpal_vbs_dep = declare_dependency(
    link_with: libpal_vbs,
)
