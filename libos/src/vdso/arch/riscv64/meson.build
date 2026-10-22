c_vdso_args = [
    '-fno-stack-protector',
    '-fPIC',
    '-ffreestanding',
]

if vm
    c_vdso_args += ['-DVDSO_USE_SYSCALL_INSTR']
endif

# No -mgeneral-regs-only analog to x86_64's `-mno-sse` here: riscv64's F/D registers are part of
# the lp64d ABI the whole tree is built for, and the vDSO's `ecall` sites are entered with full
# app state (the seccomp-trap path saves/restores all registers via the LibOS context), so FP
# codegen in clock_gettime's helper paths is harmless. (A vector-using compile would *not* be
# -- but riscv64 gcc's default -march (rv64imafdc_zicsr_zifencei) cannot emit V instructions at
# all, see dev docs: riscv/PLAN.md decisions on hiding V.)
vdso_so_dbg = shared_library('vdso',
    'vdso.c',
    'vdso_note.S',
    saguaro_hash_header,

    name_prefix: '',
    name_suffix: 'so.dbg',

    # TODO this should only include libos_entry_api.h and files defining necessary types
    include_directories: includes_libos,

    c_args: c_vdso_args,
    link_args: [
        '-nostdlib',
        '-Wl,-T@0@'.format(meson.current_source_dir() / 'vdso.lds'),
        '-Wl,--hash-style=both',
        '-Wl,-Bsymbolic',
        '-Wl,--no-undefined',
        '-Wl,-zmax-page-size=4096',
        '-Wl,-zcommon-page-size=4096',
        '-Wl,--soname=linux-vdso.so.1',
    ],
    link_depends: ['vdso.lds'],
)

# Strip symbols and debug info.
#
# TODO: It would be nice to keep debug info if we're in a debug build, but this seems to prevent GDB
# from displaying a proper backtrace.
vdso_not_checked_so = custom_target('vdso_not_checked.so',
    command: [objcopy, '-S', '@INPUT@', '@OUTPUT@'],
    input: vdso_so_dbg,
    output: 'vdso_not_checked.so',
)

# vdso.so is required to have no relocations. This rule checks it, and if good, copies to vdso.so.
#
# RISC-V note: check-no-reloc.sh runs the *build machine's* `readelf` (found via PATH inside the
# cross container, which has binutils for both architectures; readelf is target-agnostic and
# reads riscv64 ELF fine). What must *not* appear here are the GOT relocations a `la` pseudo-op
# would otherwise leave behind -- vdso.c's vdso_arch_syscall() uses a raw `ecall` (see
# vdso_syscall.h), so no relocation-generating symbol reference exists in this object at all,
# same guarantee the aarch64 port documented there.
vdso_so = custom_target('vdso.so',
    command: [check_no_reloc_prog, '@INPUT@', '@OUTPUT@'],
    input: vdso_not_checked_so,
    output: 'vdso.so',
)
