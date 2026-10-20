# TODO

Open items by architecture and host. Longer lists are kept with the maintainers' development notes.

## x86_64

- `direct`: audit PAL state that a starting child allocates and frees at exit; sound and the remaining GPU-driver gaps for graphics.
- `vm`: multi-vCPU performance at 2 and 4 vCPUs (fork cost, TLB shootdowns, scheduler lock contention); robust mutex for the System V shared tables.
- `vm` with graphics: present events and a non-flipping blit for Vulkan, idle CPU of the listener, SuperTuxKart start hang, sound.
- `sev`: trusted files, Secure TSC; `sgx`: remote attestation (AESM, PCCS) and the `ra-tls` examples; `tdx`: never run on this tree's hardware, needs `td-shim` and a TDX QEMU.
- Proton (Wine, DXVK) under `gramine-vm`: phase 1 and later, see the status in the development notes.

## AArch64

- `direct`: the `kill11` hang; longer soak runs and real applications (openssh, dropbear); ABI-conformance tests.
- `vm`: the guest cannot map distribution programs (`gcc`, `openjdk`, `pytorch`, `python` fail); the pipe round trips at 4 vCPUs.
- Graphics: several GPU nodes, Vulkan and CUDA untested; `vm` with graphics is not available.
- CPU feature words: SVE, PAC, BTI and MTE stay masked until the LibOS supports them.

## RISC-V (RV64)

- `direct`: validation on hardware (stress under load, other example suites); the hosted `test_603` timeout; CPU identification in the PAL.
- `vm`: planned.

## Windows 11 (ARM64 and x64): VBS enclaves

- `nodejs` on ARM64 (the reserved register x18 in `libstdc++`); x64 variants of the static examples.
- `fork` (a multi-enclave design), sealing, remote attestation, production signing.
- A pure-Windows build without WSL; a prebuilt sysroot bundle.
- The `helloworld` check fails now and then right after its image is built.

## All architectures

- LibOS system calls that are unimplemented or partly implemented (by group), and the open failures of the LTP matrix.
- `exec.sh`: backends for `tdx` and the RISC-V container; `unit_tests-mremap` under SGX (a shrunk mapping stays readable).
- LibOS test `test_020_cpuinfo` should ignore host CPU flags that KVM does not pass (`mwaitx` on AMD hosts).
- CI: the vm workflow depends on hosted runners with KVM; a software-Vulkan smoke test is a candidate.
