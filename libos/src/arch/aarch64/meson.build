# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The ASTERISC Research Lab

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

# No libos_arch_prctl.c on aarch64: arch_prctl doesn't exist in this architecture's syscall ABI
# (TLS is set directly via TPIDR_EL0, no syscall involved at all -- see libos_tcb_arch.h), and
# there is correspondingly no __NR_arch_prctl entry in libos_table.c to dispatch to it.
libos_sources_arch = files()

foreach src, params : libos_sources_arch_list
    libos_sources_arch += files(src)
endforeach

libos_lds = meson.current_source_dir() / 'libos.lds'
