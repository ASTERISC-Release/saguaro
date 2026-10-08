# The x86_64 suite here (libos/test/regression/asm/x86_64/) tests IRET-instruction emulation,
# which only exists on PAL/Linux-SGX (see test_x86_64.py's own comment) -- `iret` is an x86-only
# instruction with no aarch64 analog, and the SGX backend is permanently out of scope for this
# port (Intel-only ISA extension). This file exists only to satisfy the unconditional
# `subdir(host_machine.cpu_family())` above; it intentionally defines no test targets.
