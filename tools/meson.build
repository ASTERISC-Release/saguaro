gramine_vcs_tag = vcs_tag(
    command: ['git', 'rev-parse', '--verify', 'HEAD'],
    input: 'vcs_tag.in',
    output: 'vcs_tag',
)

common_render_defines = [
    '--config', gramine_vcs_tag,
    '-D', 'VERSION=@0@'.format(meson.project_version()),
    '-D', 'PREFIX=@0@'.format(get_option('prefix')),
    '-D', 'BINDIR=@0@'.format(get_option('bindir')),
]

if direct
    hostpalpath_direct = get_option('prefix') / pkglibdir / 'direct'
    custom_target('gramine-direct',
        command: [meson_render_script_prog,
            '--mode', '755',
            common_render_defines,
            '-D', 'SGX=0',
            '-D', 'HOST_PAL_PATH=@0@'.format(hostpalpath_direct),
            '-D', 'LIBPAL_PATH=@0@'.format(hostpalpath_direct / 'libpal.so'),
            '-D', 'PAL_CMD=@0@'.format(hostpalpath_direct / 'loader'),
            '-D', 'CONFIG_SGX_DRIVER=',
            '@INPUT@',
            '@OUTPUT@',
        ],
        input: 'gramine.in',
        output: 'gramine-direct',
        install: true,
        install_dir: get_option('bindir'),
    )
endif

if sgx
    subdir('sgx')

    hostpalpath_linux_sgx = get_option('prefix') / pkglibdir / 'sgx'

    custom_target('gramine-sgx',
        command: [meson_render_script_prog,
            '--mode', '755',
            common_render_defines,
            '-D', 'SGX=1',
            '-D', 'HOST_PAL_PATH=@0@'.format(hostpalpath_linux_sgx),
            '-D', 'LIBPAL_PATH=@0@'.format(hostpalpath_linux_sgx / 'libpal.so'),
            '-D', 'PAL_CMD=@0@'.format(get_option('prefix') / pkglibdir / 'sgx' / 'loader'),
            '-D', 'CONFIG_SGX_DRIVER=@0@'.format(sgx_driver),
            '@INPUT@',
            '@OUTPUT@',
        ],
        input: 'gramine.in',
        output: 'gramine-sgx',
        install: true,
        install_dir: get_option('bindir'),
    )
endif

if vm
    hostpalpath_vm = join_paths(get_option('prefix'), pkglibdir, 'vm')
    conf_gramine_vm = configuration_data()
    conf_gramine_vm.merge_from(conf)
    conf_gramine_vm.set_quoted('HOST_PAL_PATH', hostpalpath_vm)
    conf_gramine_vm.set_quoted('BIOS_PATH', join_paths(hostpalpath_vm, 'bios'))
    conf_gramine_vm.set_quoted('LIBPAL_PATH', join_paths(hostpalpath_vm, 'pal'))

    # The x86 launcher (q35, BIOS blob, NUMA DIMMs, vhost-vsock) cannot work on aarch64: the
    # arm one boots the kernel under QEMU `virt` with a dumped DTB. Both install as `gramine-vm`.
    # the per-VM vsock relay of the launchers (sourced by gramine-vm-aarch64; see its header)
    install_data('gramine-vm-relay.sh', install_dir: hostpalpath_vm, rename: 'vm-relay.sh')

    configure_file(
        input: host_machine.cpu_family() == 'aarch64' ? 'gramine-vm-aarch64.in' : 'gramine-vm.in',
        output: 'gramine-vm',
        configuration: conf_gramine_vm,
        install_dir: get_option('bindir'),
        install_mode: 'rwxr-xr-x',
    )
endif

if sev
    # gramine-sev is gramine-vm.in with the paths of the SEV build of the vm PAL (pal/src/host/vm/sev)
    hostpalpath_sev = join_paths(get_option('prefix'), pkglibdir, 'sev')
    conf_gramine_sev = configuration_data()
    conf_gramine_sev.merge_from(conf)
    conf_gramine_sev.set_quoted('HOST_PAL_PATH', hostpalpath_sev)
    conf_gramine_sev.set_quoted('BIOS_PATH', join_paths(hostpalpath_sev, 'bios'))
    conf_gramine_sev.set_quoted('LIBPAL_PATH', join_paths(hostpalpath_sev, 'pal'))
    conf_gramine_sev.set_quoted('SEV_IGVM_PATH', join_paths(hostpalpath_sev, 'gramine-sev.igvm'))

    install_data('gramine-vm-relay.sh', install_dir: hostpalpath_sev, rename: 'vm-relay.sh')

    configure_file(
        input: 'gramine-vm.in',
        output: 'gramine-sev',
        configuration: conf_gramine_sev,
        install_dir: get_option('bindir'),
        install_mode: 'rwxr-xr-x',
    )
endif

if tdx
    hostpalpath_tdx = join_paths(get_option('prefix'), pkglibdir, 'tdx')
    conf_gramine_tdx = configuration_data()
    conf_gramine_tdx.merge_from(conf)
    conf_gramine_tdx.set_quoted('TDSHIM_PAL_PATH', join_paths(hostpalpath_tdx, 'tdshim-pal'))

    # gramine-tdx is gramine-vm.in with the TDX paths: it sources the same relay library
    install_data('gramine-vm-relay.sh', install_dir: hostpalpath_tdx, rename: 'vm-relay.sh')

    configure_file(
        input: 'gramine-vm.in',
        output: 'gramine-tdx',
        configuration: conf_gramine_tdx,
        install_dir: get_option('bindir'),
    )
endif

executable('gramine-argv-serializer',
    'argv_serializer.c',
    install: true)
