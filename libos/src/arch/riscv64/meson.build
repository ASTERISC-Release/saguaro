libos_sources_arch_list = {
    'fs_proc/info.c': {},
    'libos_context.c': {},
    'libos_cpuid.c': {},
    'libos_elf_entry.S': {},
    'libos_syscall_patch.c': {},
    'libos_table.c': {},
    'start.S': {},
    'syscallas.S': {},
}

# No libos_arch_prctl.c on riscv64 (same reason as the aarch64 sibling): there is no
# arch_prctl syscall in this architecture's syscall ABI at all -- TLS is installed directly via
# the user-writable `tp` register (see libos_tcb_arch.h's set_tls()) -- and there is
# correspondingly no __NR_arch_prctl entry in libos_table.c to dispatch to.
libos_sources_arch = files()

# Wrappers that narrow every syscall argument to its declared type before the call (see
# ../gen_syscall_wrappers.py): the RISC-V calling convention lets compiled code assume a 32-bit
# argument is sign-extended, which the syscall ABI does not guarantee. libos_table.c includes the
# generated header, so the table points at the wrappers.
libos_sources_arch += custom_target('libos_syscall_wrappers.h',
    input: [meson.current_source_dir() / '../gen_syscall_wrappers.py',
            meson.project_source_root() / 'libos/include/libos_table.h'],
    output: 'libos_syscall_wrappers.h',
    command: [python3, '@INPUT0@', '@INPUT1@', '@OUTPUT@', '__riscv'],
)

foreach src, params : libos_sources_arch_list
    libos_sources_arch += files(src)
endforeach

libos_lds = meson.current_source_dir() / 'libos.lds'
