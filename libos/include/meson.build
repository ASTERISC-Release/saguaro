includes_libos = [
    include_directories(
        '.',
        'arch/@0@'.format(host_machine.cpu_family()),
    ),
    includes_pal_for_libos,
    common_inc,
]

libos_generated_offsets_print = executable('libos-generated-offsets-print',
    'generated_offsets.c',
    generated_offsets_print_src,
    include_directories: includes_libos,
    c_args: cflags_libos,
)

# Under a cross build this program is a riscv64 binary executed on the build machine, so
# it runs under the cross file's exe_wrapper (qemu-riscv64 user-mode); empty natively.
libos_asm_offsets_h = custom_target('asm_offsets.h[libos]',
    command: saguaro_exe_wrapper + [libos_generated_offsets_print, '--h'],
    capture: true,
    output: 'asm_offsets.h',
)

gramine_hash_header = vcs_tag(
    command: ['git', 'rev-parse', '--verify', 'HEAD' ],
    input: 'gramine_hash.h.in',
    output: 'gramine_hash.h',
    replace_string: '@GRAMINE_COMMIT_HASH_MARKER@',
)
