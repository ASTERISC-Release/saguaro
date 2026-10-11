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

foreach src, params : libos_sources_arch_list
    libos_sources_arch += files(src)
endforeach

libos_lds = meson.current_source_dir() / 'libos.lds'
