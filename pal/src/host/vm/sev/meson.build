# AMD SEV / SEV-SNP build of the x86-64 vm PAL (Documentation/devel/sev/PLAN.md D2): the same
# sources as ../meson.build, compiled again with PAL_HOST_SEV, plus the SEV files of this directory,
# installed into <pkglibdir>/sev next to the IGVM file that boots it. HOST_TYPE stays VM: the LibOS
# treats this PAL exactly like the vm PAL. The vm PAL of ../meson.build is not affected by this file.

pal_sev_bios = executable('bios',
    pal_vm_bios_src,

    c_args: ['-DBIOS_SEV_IGVM=1'],
    link_args: [
        '-nostdlib',
        '-static',
        '-Wl,-T@0@'.format(pal_vm_bios_lds),
    ],
    link_depends: pal_vm_bios_lds,

    install: true,
    install_dir: pkglibdir / 'sev',
)

pal_sev_sources = files(
    'sev.c',
    'sev_attest.c',
    'sev_callbacks.c',
    'sev_vc_entry.S',
)

pal_sev_inc = [
    includes_pal_common,
    pal_vm_common_inc,
    include_directories('..'),
    include_directories('.'),
]

# The #VC handler and the GHCB calls: they run inside any context, user mode included, and save
# no FPU state, so they must not use SSE registers (-mgeneral-regs-only) nor the stack protector
# (its canary lives in %gs, which may be the application's)
pal_sev_vc_lib = static_library('pal_sev_vc',
    'sev_vc.c',
    'sev_ghcb.c',
    pal_vm_common_asm_offsets_h,
    include_directories: pal_sev_inc,
    c_args: [
        cflags_pal_common,
        '-DHOST_TYPE=VM',
        '-DPAL_HOST_VM=1',
        '-DPAL_HOST_SEV=1',
        '-mno-red-zone',
        '-mgeneral-regs-only',
        '-fno-stack-protector',
        '-Wno-address-of-packed-member',
    ],
    # headers only: common_dep also carries the common sources, which the PAL links itself
    dependencies: [common_dep.partial_dependency(includes: true, compile_args: true)],
    pic: true,
)

libpal_sev = executable('pal',
    pal_vm_sources,
    pal_vm_common_sources,
    pal_vm_common_nontdx_sources,
    pal_vm_common_asm_offsets_h,
    pal_common_sources,
    pal_sev_sources,

    include_directories: pal_sev_inc,
    link_whole: pal_sev_vc_lib,

    c_args: [
        cflags_pal_common,
        '-DHOST_TYPE=VM',
        '-DPAL_HOST_VM=1',
        '-DPAL_HOST_SEV=1',
        '-mno-red-zone',
        '-Wno-address-of-packed-member',
        cflags_custom_stack_protector,
        cflags_sanitizers,
    ],

    link_args: [
        '-nostdlib',
        '-Wl,--hash-style=sysv',
        '-Wl,-soname=libpal.so',
        '-Wl,-zrelro',
        '-Wl,-znow',
        '-Wl,-zcombreloc',
        '-Wl,-zdefs',
        '-Wl,--version-script=@0@'.format(pal_vm_map.full_path()),
        '-Wl,-T@0@'.format(pal_vm_lds),
        '-Wl,--eh-frame-hdr',
    ],

    pie: true,
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
    install_dir: pkglibdir / 'sev',
)

# The boot image: the PAL, the SEV reset stub and (for SNP) the initial VMSA in one IGVM file that
# QEMU loads with -object igvm-cfg (PLAN.md D4); every page of it is measured at launch.
pal_sev_igvm_prog = find_program('gramine-sev-igvm.py')
custom_target('gramine-sev.igvm',
    command: [pal_sev_igvm_prog, 'build', '--pal', libpal_sev, '--bios', pal_sev_bios,
              '--output', '@OUTPUT@'],
    output: 'gramine-sev.igvm',
    install: true,
    install_dir: pkglibdir / 'sev',
)

install_data('gramine-sev-igvm.py', install_dir: pkglibdir / 'sev', install_mode: 'rwxr-xr-x')
