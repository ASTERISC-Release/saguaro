if skeleton
    libpal = libpal_skeleton_dep
elif direct
    libpal = libpal_direct_dep
elif sgx
    libpal = libpal_sgx_dep
elif vm
    libpal = libpal_vm_dep
elif tdx
    libpal = libpal_tdx_dep
else
    error('need to link LibOS against a PAL library, but no PAL version is enabled')
endif

libos_sources = files(
    'bookkeep/libos_handle.c',
    'bookkeep/libos_pid.c',
    'bookkeep/libos_process.c',
    'bookkeep/libos_signal.c',
    'bookkeep/libos_thread.c',
    'bookkeep/libos_vma.c',
    'fs/chroot/encrypted.c',
    'fs/chroot/fs.c',
    'fs/dev/attestation.c',
    'fs/dev/fs.c',
    'fs/etc/fs.c',
    'fs/eventfd/fs.c',
    'fs/libos_dcache.c',
    'fs/libos_fs.c',
    'fs/libos_fs_encrypted.c',
    'fs/libos_fs_hash.c',
    'fs/libos_fs_lock.c',
    'fs/libos_fs_mem.c',
    'fs/libos_fs_path.c',
    'fs/libos_fs_pseudo.c',
    'fs/libos_fs_synthetic.c',
    'fs/libos_fs_util.c',
    'fs/libos_namei.c',
    'fs/pipe/fs.c',
    'fs/proc/fs.c',
    'fs/proc/info.c',
    'fs/proc/ipc_thread.c',
    'fs/proc/thread.c',
    'fs/shm/fs.c',
    'fs/socket/fs.c',
    'fs/sys/cache_info.c',
    'fs/sys/cpu_info.c',
    'fs/sys/fs.c',
    'fs/sys/node_info.c',
    'fs/tmpfs/fs.c',
    'gramine_hash.c',
    'ipc/libos_ipc.c',
    'ipc/libos_ipc_child.c',
    'ipc/libos_ipc_fs_lock.c',
    'ipc/libos_ipc_pid.c',
    'ipc/libos_ipc_process_info.c',
    'ipc/libos_ipc_signal.c',
    'ipc/libos_ipc_sync.c',
    'ipc/libos_ipc_vmid.c',
    'ipc/libos_ipc_worker.c',
    'libos_async.c',
    'libos_call.c',
    'libos_checkpoint.c',
    'libos_debug.c',
    'libos_init.c',
    'libos_malloc.c',
    'libos_object.c',
    'libos_parser.c',
    'libos_pollable_event.c',
    'libos_rtld.c',
    'libos_rwlock.c',
    'libos_syscalls.c',
    'libos_utils.c',
    'net/ip.c',
    'net/unix.c',
    'sync/libos_sync_client.c',
    'sync/libos_sync_server.c',
    'sys/libos_access.c',
    'sys/libos_alarm.c',
    'sys/libos_brk.c',
    'sys/libos_capabilities.c',
    'sys/libos_clone.c',
    'sys/libos_dup.c',
    'sys/libos_epoll.c',
    'sys/libos_eventfd.c',
    'sys/libos_exec.c',
    'sys/libos_exit.c',
    'sys/libos_fcntl.c',
    'sys/libos_file.c',
    'sys/libos_fork.c',
    'sys/libos_futex.c',
    'sys/libos_getcwd.c',
    'sys/libos_getpid.c',
    'sys/libos_getrandom.c',
    'sys/libos_getrlimit.c',
    'sys/libos_getuid.c',
    'sys/libos_ioctl.c',
    'sys/libos_mlock.c',
    'sys/libos_mmap.c',
    'sys/libos_memfd.c',
    'sys/libos_open.c',
    'sys/libos_pipe.c',
    'sys/libos_poll.c',
    'sys/libos_sched.c',
    'sys/libos_sem.c',
    'sys/libos_sysv_shm.c',
    'sys/libos_sigaction.c',
    'sys/libos_sleep.c',
    'sys/libos_socket.c',
    'sys/libos_stat.c',
    'sys/libos_time.c',
    'sys/libos_uname.c',
    'sys/libos_wait.c',
    'sys/libos_wrappers.c',
    'utils/log.c',

    'sys/libos_prctl.c',
    'sys/libos_mremap.c',
    'sys/libos_getresid.c',
    'sys/libos_graphics_lock.c'
)

# Arch-specific meson.build must define the following Meson variables:
#   - `libos_sources_arch` - a list of arch-specific sources,
#   - `libos_lds` - a full path to LibOS linker script.
subdir('arch')
libos_sources += libos_sources_arch

libos_map = meson.current_source_dir() / 'libos.map'

subdir('vdso')

vdso_data_o = custom_target('vdso_data.o',
    input: 'vdso/vdso_data.S',
    output: 'vdso_data.o',
    command: [
        cc.cmd_array(),
        cflags_libos,
        '-Wa,--noexecstack',
        '-DVDSO_SO_FULL_PATH="@0@"'.format(vdso_so.full_path()),
        '-c',
        '@INPUT@',
        '-o', '@OUTPUT@',
    ],
    depends: vdso_so,
)

# Compile-time switch for the Tirith graphics paths (see meson_options.txt).
cflags_graphics = []
if get_option('graphics') == 'enabled'
    cflags_graphics += '-DENABLE_GRAPHICS'
endif

# riscv64: riscv gcc compiles __builtin_bswap32/64 (used heavily by mbedtls's
# big-endian wire-format code) into libgcc helper calls -- `__bswapsi2`/`__bswapdi2`
# -- because the tree is compiled for the baseline riscv ISA (the toolchain's
# default `-march`, rv64imafdc_zicsr_zifencei, has no byte-swap instruction; the
# Zbb extension that provides them is not part of the default -- even where the
# test guest's CPU implements it, see the Phase 0 log). Gramine links libsysdb.so
# with -nostdlib, which excludes even libgcc.a -- so it must be linked explicitly. This pulls in only the few
# helper objects actually referenced (static archive semantics); it does not drag
# in any libc (libgcc.a is compiler-internal, has no libc dependencies). x86_64 and
# aarch64 have byte-swap instructions (bswap on x86; rev on aarch64), never emit
# these calls, and keep an empty list here.
libsysdb_gcc_deps = host_machine.cpu_family() == 'riscv64' ? [
    cc.find_library('gcc', static: true),
] : []

libos = shared_library('sysdb',
    libos_sources,
    vdso_data_o,
    libos_asm_offsets_h,
    gramine_hash_header,

    c_args: [
        cflags_libos,
        cflags_sanitizers,
        cflags_graphics,

        # aarch64 + VM PAL (Milestone B): LibOS runs in EL0 of the unikernel, where TPIDR_EL1
        # (the kernel's per-CPU base holding cur_tcb) is unreadable. pal_arch.h's pal_get_tcb()
        # then resolves the kernel-maintained g_pal_tcb_singleton mirror instead of the
        # tagged-TPIDR/registry machinery of the Linux PAL; see
        # pal/src/host/vm-common/arch/aarch64/pal_vm_tcb.c for the contract the kernel keeps.
        # (x86_64 needs nothing of the sort: %gs-relative addressing works at ring-3.)
        vm and host_machine.cpu_family() == 'aarch64' ? ['-DHOST_TYPE=VM', '-DPAL_HOST_VM=1'] : [],

        # aarch64 + VM PAL (Milestone B): common_dep passes its sources (memcpy/memset/etc.)
        # through to this target, so they are compiled with these flags. -O3 would
        # auto-vectorize those byte loops into 128-bit ldr q/str q accesses, which
        # architecturally fault when unaligned and have no host fixup handler at EL0 of the
        # unikernel (under direct-PAL, Linux's alignment fixup rescues them). Same reason as
        # in pal/src/host/vm/meson.build.
        vm and host_machine.cpu_family() == 'aarch64' ? ['-fno-tree-vectorize'] : [],

        # use TLS-based stack protector of GCC (we rely on the fact that LibOS reuses the same TCB
        # as its underlying PAL which must have a canary in its PAL_TCB at offset 0x8, so no
        # additional enabling is required in the LibOS code)
        cflags_custom_stack_protector,
    ],
    link_args: [
        '-nostdlib',

        # PAL expects that LibOS binary has old-style hash (DT_HASH) table instead of the default
        # GNU hash (DT_GNU_HASH) table; PAL symbol resolution uses DT_HASH to figure out the number
        # of dynamic symbols in the SYMTAB symbol table
        '-Wl,--hash-style=sysv',

        # PAL expects that LibOS binary is linked with RELRO (Relocation Read-Only)
        '-Wl,-zrelro',
        '-Wl,-znow',

        '-Wl,-zcombreloc',
        '-Wl,--no-undefined',

        # aarch64 + VM PAL (Milestone B): libsysdb.so is linked against the `pal` *executable*
        # (link_with: libpal_vm below). x86_64 ld happily resolves undefined references from a
        # PIE executable's .dynsym, but aarch64 ld rejects executables as link inputs outright
        # ("cannot use executable file as input to a link"). This is purely a build-time
        # limitation: at runtime pal_rtld resolves every UNDEF of libsysdb.so against the PAL's
        # exported symbol table anyway (find_symbol_in_loaded_maps() in pal/src/pal_rtld.c
        # searches g_pal_map.symbol_table for exactly this, including the aarch64-only
        # g_pal_tcb_singleton/libos_tcb_registry_* cells), so leaving the references undefined
        # in the DSO is semantically identical -- any symbol the two binaries disagree on still
        # fails loudly at libsysdb load time, just one phase later than ld would have.
        vm and host_machine.cpu_family() == 'aarch64'
            ? ['-Wl,--unresolved-symbols=ignore-in-object-files'] : [],
        '-Wl,--version-script=@0@'.format(libos_map),
        '-Wl,-T@0@'.format(libos_lds),
        '-Wl,--eh-frame-hdr',
    ],
    link_depends: [
        libos_map,
        libos_lds,
    ],
    dependencies: [
        common_dep,
        cryptoadapter_dep,
        protected_files_dep,
        libpal,
        libsysdb_gcc_deps,
    ],

    include_directories: includes_libos,
    install: true,
    install_dir: pkglibdir,
)

if debug
    custom_target('libsysdb.so.map',
        input: libos,
        command: gen_symbol_map_cmd,
        capture: true,
        output: 'libsysdb.so.map',

        install: true,
        install_dir: pkglibdir,
    )
endif

# The graphics preload library is only usable under a VM-type PAL, and its sources are x86_64-only
# for now (rdtscp timing code).
if get_option('graphics') == 'enabled' and (vm or tdx) and host_machine.cpu_family() == 'x86_64'
    subdir('graphics')
endif
