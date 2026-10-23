# TODO

Open items by architecture and host. Longer lists are kept with the maintainers' development notes.

## x86_64

- `direct`: audit PAL state that a starting child allocates and frees at exit; sound and the remaining GPU-driver gaps for graphics.
- `vm`: multi-vCPU performance at 2 and 4 vCPUs (fork cost, TLB shootdowns, scheduler lock contention); robust mutex for the System V shared tables.
- `vm` with graphics: present events and a non-flipping blit for Vulkan, idle CPU of the listener, SuperTuxKart start hang, sound.
- `vm` with graphics, after the pointer table and the guard:
  soak tests (DOOM on the AMD GPU at least 20 runs, each Proton game at least 5 runs per GPU); the caller of the
  260-byte over-read in Wine's Unix side (ntdll, Mesa or the presentation layer) is unidentified; the AMD
  "Internal memory fault at 0x10" on the EGL path is fixed (`lseek`/`fstat` on identity host fds), but the
  registration of raw host fd numbers as LibOS fds still collides with the application's fds by chance; the i915 table refuses
  `I915_EXEC_FENCE_IN`, engine maps with extensions and user-pointer objects, and the `xe` driver is not covered;
  an AArch64 build check of the changes; equal-scene frame-rate comparisons for The Witcher and BioShock 2.
- `vm` with graphics: qemu-tirith will be published: give it a stable tag, update the clone URLs in the guides, and decide whether to pin it as a git submodule (a pinned commit under a fixed path would remove the `QEMU_TIRITH_DIR` step; a submodule needs `--recursive` and a 1+ GB build tree) or keep a documented separate checkout.
- CI workflows, later: run the rewrite stress step by default on direct aarch64 and add one for riscv64; make the LibOS step of the x86_64 VM workflow strict (it is informational); run the full Python example (with its HTTP check) in the x86_64 VM workflow; make Python work in the aarch64 TCG workflow (the system Python is not PIE).
- CI for hardware-bound backends (`sev`, `sgx`, `vbs`, `tdx`): there is no workflow and no self-hosted runner, deliberately (the repository is public); `test/sev/test-sev-all.sh` and the `./exec.sh --<backend>` runs are done by hand on the hardware.
- `sev`: trusted files, Secure TSC; `sgx`: remote attestation (AESM, PCCS) and the `ra-tls` examples.
- Proton (Wine, DXVK) under `saguaro-vm`: phase 1 and later, see the status in the development notes.

## AArch64

- `direct`: the `kill11` hang; longer soak runs and real applications (openssh, dropbear); ABI-conformance tests.
- `vm` under TCG (software emulation, the `VM PAL (aarch64, TCG)` workflow): `test_604_sigreturn_regs` hangs on the hosted arm64 runner (it passes under TCG on a local aarch64 machine, and under KVM); it is deselected in the workflow until the cause is found (a timing-dependent asynchronous-signal return, x17 and the flags).
- `vm`: the guest cannot map distribution programs (`gcc`, `openjdk`, `pytorch`, `python` fail); the pipe round trips at 4 vCPUs.
- Graphics: several GPU nodes, Vulkan and CUDA untested; `vm` with graphics is not available.
- CPU feature words: SVE, PAC, BTI and MTE stay masked until the LibOS supports them.

## RISC-V (RV64)

- `direct`: the stress test under load on the board; `test_103_gettimeofday` needs about 15 s of the harness's 10 s default (the host clock path, not the rewriting); `test_2025_fork_reservation` fails with `ENOMEM` on a 3.7 GiB board; the hosted `test_603` timeout (it passes natively); CPU identification in the PAL.
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
