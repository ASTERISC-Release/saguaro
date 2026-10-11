project('glibc', 'c', version: '2.39')

fs = import('fs')

glibc_output_paths = [
    'csu/crt1.o',
    'csu/crti.o',
    'csu/crtn.o',
    'dlfcn/libdl.so',
    'dlfcn/libdl.so.2',
    'libc.so',
    'libc.so.6',
    'login/libutil.so',
    'login/libutil.so.1',
    'math/libm.so',
    'math/libm.so.6',
    'nis/libnsl.so',
    'nis/libnsl.so.1',
    'nptl/libpthread.so',
    'nptl/libpthread.so.0',
    'nptl_db/libthread_db.so',
    'nptl_db/libthread_db.so.1',
    'nss/libnss_compat.so',
    'nss/libnss_compat.so.2',
    'nss/libnss_db.so',
    'nss/libnss_db.so.2',
    'nss/libnss_files.so',
    'nss/libnss_files.so.2',
    'resolv/libanl.so',
    'resolv/libanl.so.1',
    'resolv/libnss_dns.so',
    'resolv/libnss_dns.so.2',
    'resolv/libresolv.so',
    'resolv/libresolv.so.2',
    'rt/librt.so',
    'rt/librt.so.1',
]

if host_machine.cpu_family() == 'x86_64'
    glibc_output_paths += [
        'elf/ld-linux-x86-64.so.2',
        'elf/ld.so',
        'mathvec/libmvec.so',
        'mathvec/libmvec.so.1',
    ]
elif host_machine.cpu_family() == 'aarch64'
    # Same set as x86_64: the loader (Gramine's runtime /lib chroot has no other
    # ld-*; find_interp() resolves every test binary's PT_INTERP against it) and
    # libmvec (built for aarch64 since glibc 2.28, in the same mathvec/ dir).
    glibc_output_paths += [
        'elf/ld-linux-aarch64.so.1',
        'elf/ld.so',
        'mathvec/libmvec.so',
        'mathvec/libmvec.so.1',
    ]
elif host_machine.cpu_family() == 'riscv64'
    # The loader (same find_interp()/runtime-chroot reasoning as the other two branches;
    # glibc's riscv64 loader carries the ABI in its soname -- lp64d, the only ABI Gramine
    # targets, see Documentation/devel/riscv/plan.md decisions).
    #
    # No mathvec/ entries, unlike both other branches: glibc 2.39 only builds libmvec on
    # x86_64 and aarch64 (`SUBDIRS-maths` in sysdeps/meson configure machinery); riscv64
    # libmvec support landed in much later glibc releases only. riscv64 glibc's math.h
    # therefore has no vectorized entry points for Gramine's chroot to be missing.
    glibc_output_paths += [
        'elf/ld-linux-riscv64-lp64d.so.1',
        'elf/ld.so',
    ]
endif

glibc_output = []
foreach path : glibc_output_paths
    glibc_output += fs.name(path)
endforeach

extra_cflags = ''
if get_option('buildtype') == 'debug' or get_option('buildtype') == 'debugoptimized'
    extra_cflags += '-g'
endif

if get_option('vm') == 'enabled' or get_option('tdx') == 'enabled'
    compile_prog = find_program('compile-no-patches.sh')
else
    compile_prog = find_program('compile.sh')
endif

# Cross-building (riscv64 port, see Documentation/devel/riscv/plan.md): custom targets
# always run on the *build* machine, so compile.sh would default to the native x86_64 gcc
# and build an x86_64 glibc. Pass the cross C/C++ compilers (from the cross file's
# [binaries]) and a --host triple through the environment; compile.sh's `CC=gcc` etc. are
# `:=`-style defaults precisely so this env wins, and `CONFIGURE_HOST` makes glibc's
# configure switch to cross mode (it is then careful never to *run* any built target
# binary, only compile-test -- and `--disable-test` drops the test suite entirely).
glibc_env = []
if meson.is_cross_build()
    glibc_cc = meson.get_compiler('c')
    glibc_env = [
        'CC=' + ' '.join(glibc_cc.cmd_array()),
        'AS=' + ' '.join(glibc_cc.cmd_array()),
    ]
    # CXX unconditionally: glibc still *compiles* (never runs) its C++ support
    # programs, and without it make would fall back to the native g++ and fail on
    # the riscv sysdeps headers. No `required:` kwarg on get_compiler() (meson
    # >=1.4 only; Gramine's floor is 0.56); the top-level project declares C++
    # anyway, so a host cpp always exists in any Gramine build.
    glibc_env += 'CXX=' + ' '.join(meson.get_compiler('cpp').cmd_array())
    # riscv64-linux-gnu is the canonical --host triple for riscv64 glibc; deriving it from
    # cpu_family happens to spell exactly the right triple for riscv64 (and would for
    # aarch64/x86_64 too, should those ever be cross-built from here).
    glibc_env += 'CONFIGURE_HOST=' + host_machine.cpu_family() + '-linux-gnu'
endif

glibc = custom_target('glibc',
    env: glibc_env,
    command: [
        compile_prog,
        host_machine.cpu_family(),
        '@CURRENT_SOURCE_DIR@',
        '@CURRENT_SOURCE_DIR@/glibc-2.39',
        meson.current_build_dir(),
        '@PRIVATE_DIR@',
        get_option('prefix'),
        get_option('libdir'),
        extra_cflags,
        glibc_output_paths,
    ],

    input: [
        'glibc-2.39/configure',
        'fix-fortify-source.patch',
        'gramine-syscall.patch',
        'hp-timing.patch',
    ],

    output: glibc_output,

    console: true,

    install: true,
    install_dir: get_option('libdir') / 'gramine/runtime/glibc',
)
