cflags_libos = [
    '-fPIC',
    # TODO: check whether all standard functions implemented in Gramine match their APIs and remove
    # this
    '-ffreestanding',

    '-DIN_LIBOS',
]

cflags_libos += cc.get_supported_arguments(
    # Some of the code uses alignof on expressions, which is a GNU extension.
    # Silence Clang - it complains but does support it.
    '-Wno-gnu-alignof-expression',
)

if host_machine.cpu_family() == 'x86_64'
    # instruct GCC not to use the AMD64 ABI 128-byte redzone which resides below the stack
    # pointer, or kernel will be interrupt unsafe (https://wiki.osdev.org/Creating_a_64-bit_kernel)
    # -- aarch64 (AAPCS64) has no red zone concept at all, so there's no equivalent flag to pass
    # (see RED_ZONE_SIZE in linux_abi/signals_arch.h, which is 0 there for the same reason).
    cflags_libos += ['-mno-red-zone', '-mfxsr', '-mxsave']
endif

subdir('include')
subdir('src')

if enable_tests
    subdir('test')
endif
