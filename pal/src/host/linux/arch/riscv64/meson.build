# No pal_arch_misc.c on riscv64: its x86_64 counterpart only implements _PalSegmentBaseGet/Set
# (behind arch_prctl, which doesn't exist on riscv64 -- TLS is set directly via the `tp` register,
# see pal_host_arch.h) and _PalCpuIdRetrieve (a thin cpuid() wrapper; the generic
# PalCpuIdRetrieve/PalSegmentBaseGet/Set public APIs that would call these are themselves
# `#ifdef __x86_64__` in pal/src/pal_misc.c, so none of this is reachable on riscv64 at all.
pal_direct_sources_arch = files(
    'pal_arch_exception.c',
    'pal_start.S',
)

pal_direct_lds = meson.current_source_dir() / 'pal.lds'
