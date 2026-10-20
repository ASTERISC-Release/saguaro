# Changelog

This tree extends [gramine-tdx](https://github.com/gramineproject/gramine-tdx), the Intel TDX and
virtual-machine fork of Gramine, with additional platform adaptation layers (PALs), architectures and
tooling. This document lists what the tree adds, by area. For each area it states the contribution, how to use
it and its known limits. Status words follow [README.md](README.md): *done* (works and is tested here),
*experimental* (works in the tested setups, with known gaps), *planned*. The whole tree is experimental and not
for production use. Open items are in [TODO.md](TODO.md); measured results of the examples are in
[CI-Examples/RESULTS.md](CI-Examples/RESULTS.md).

## Platform support

| PAL | Intel x86_64 | AMD64 | AArch64 | RISC-V (RV64) |
|---|---|---|---|---|
| `direct` | done | done | done | experimental (emulated guest) |
| `direct`, host GPU graphics | experimental | experimental | experimental | not available |
| `vm` | done | experimental | experimental | planned |
| `vm`, redirected graphics | experimental | experimental | planned | not available |
| `sev`, `sev-snp` | not applicable | experimental | not applicable | not applicable |
| `sgx` | experimental | not applicable | not applicable | not applicable |
| `tdx` | experimental (untested here) | not applicable | not applicable | not applicable |
| `vbs` | experimental | experimental | experimental | not applicable |

## Direct PAL on AArch64 and RISC-V

- **AArch64.** `gramine-direct` runs on AArch64 Linux (LibOS regression about 195 passed, 36 skipped; PAL
  regression 28 passed, 8 skipped; twelve example suites).
- **RISC-V.** The direct PAL is cross-built and passes the regression suites and eleven example suites in an
  emulated guest (`scripts/riscv64/`). A run on an 8-hart board completed the basic suites; its one failure
  is caused by the board's memory size.
- **System calls without a patched libc.** The first trap at an `svc` or `ecall` site is rewritten into a
  branch to a stub (`sys.rewrite_syscalls`, on by default). On the RISC-V board, a rewritten call costs
  1.9 to 2.6 us against 7.8 to 8.4 us for the trap.
- **Host libc.** With `libc=none`, no glibc is built and applications use the host's. This is the default for VM
  builds and for the direct AArch64 and RISC-V builds; x86_64 direct keeps the patched glibc. The VM build in CI
  dropped from 5 min 45 s to 1 min 19 s.
- **Limits.** RISC-V has not been validated on hardware beyond the board run above.

## VM PAL (`gramine-vm`)

- **Boot and execution.** A microVM runs under stock QEMU with KVM, at 1 to 4 vCPUs, on x86_64 and AArch64.
  The x86_64 driver stack (vsock, PCI/ivshmem, uncacheable shared mappings) was reworked.
- **Processes.** With `sys.experimental__multiprocess`, the guest supports `fork`, `vfork`, `execve`, `wait`,
  signals, pipes, shared futexes, `/dev/shm` and System V IPC. Shells, nginx workers and Redis `BGSAVE` run on
  x86_64; on AArch64, spawn, `fork` and `wait` are supported.
- **Networking.** TCP, UDP and DNS are carried over vsock to one relay process per VM, configured by the
  manifest's `vm.net` (outbound policy, inbound forwards). The relay reaches about 12 % (AArch64) to 36 % (x86_64)
  of native Redis throughput.
- **Timers.** The one-shot timer of CPU 0 is armed for the earliest pending timeout; the 100 ms tick is only a
  ceiling.
- **Use.** `./exec.sh --vm` in an example directory; guide: [quickstart/vm.md](Documentation/quickstart/vm.md).
- **Limits.** Four-vCPU `fork` performance on AArch64 and the AArch64 guest's handling of some distribution
  binaries (`gcc`, `openjdk`, `pytorch`, `python`) are open.

## AMD SEV and SEV-SNP (`gramine-sev`)

- **Contribution.** The VM PAL runs as a SEV or SEV-SNP guest, launched from an IGVM file whose measurement
  covers the guest kernel. A SEV-SNP guest obtains a hardware-signed attestation report, which is verified
  against AMD's certificate chain.
- **Validation.** The LibOS suite passes except `test_020_cpuinfo` (the host exposes `mwaitx`, which the KVM guest
  does not); all examples pass in both modes.
- **Use.** `./exec.sh --sev` or `--sev-snp`; guide: [quickstart/sev.md](Documentation/quickstart/sev.md).
- **Limits.** The tested host is a single AMD EPYC 7443P system.

## Intel SGX

- **Contribution.** The inherited SGX backend is built and tested again: `scripts/install.sh --sgx` installs it,
  and `container/run.sh` passes the enclave devices and the AESM socket into the rootless container.
- **Validation.** 19 of the 20 examples pass under `./exec.sh --sgx`; `unit_tests-mremap` fails because a
  shrunk mapping remains readable.
- **Use.** Guide: [quickstart/tdx-sgx.md](Documentation/quickstart/tdx-sgx.md).
- **Limits.** Remote attestation and the `ra-tls` examples are not covered.

## Windows VBS enclaves (`gramine-vbs`)

- **Contribution.** The `vbs` PAL runs an unmodified Linux application inside a virtualization-based security
  enclave on Windows 11 (ARM64 and x64). The library OS, a glibc and the application are packed into a signed
  enclave image at build time, because enclave memory cannot be made executable at run time.
- **Supported.** Threads, signals, host files, sockets, Python, servers (Redis, NGINX, memcached, lighttpd), SQLite,
  curl, a Rust server, `iperf3` and BusyBox applets, with local attestation.
- **Validation.** On ARM64, 11 of 12 example directories and all 8 static VBS examples pass; `nodejs` fails
  because `libstdc++` uses the reserved register x18.
- **Use.** `scripts/install.sh --vbs`, then `./exec.sh --vbs`; guide:
  [quickstart/vbs-windows.md](Documentation/quickstart/vbs-windows.md), manual: [vbs.md](Documentation/vbs.md).
- **Limits.** No `fork`, no run-time code generation, test-signed debug enclaves only.

## Graphics

- **Host GPU for `gramine-direct`.** Opt-in manifest keys let unmodified OpenGL (and, on x86_64, Vulkan)
  applications use the host's render node and X server. `glxgears`, Quake II, SuperTuxKart and Minetest reach
  76 to 102 % of native speed on Intel and AMD GPUs and 88 to 100 % on an NVIDIA GB10 (AArch64).
- **Redirected graphics for `gramine-vm`** (x86_64). EGL, GBM and DRM calls are forwarded through a shared page
  to a listener in QEMU that uses the host GPU; Vulkan is provided by an implicit layer. Guest memory is sized at
  launch, which reduces boot time at 8 GB from 2.9 s to 0.8 s.
- **Windows applications.** GE-Proton runs under `gramine-vm` as a feasibility result (DOOM I Enhanced reaches its
  first level in software rendering).
- **Limits.** `gramine-direct` is not a sandbox and the keys grant GPU access. Sound and several GPU nodes are
  not supported.

## LibOS system calls

- Added in rounds: `mremap`, System V semaphores, shared memory and message queues, `faccessat2`, `getresuid`,
  extended `prctl`, file times and links, vectored I/O, `memfd` with seals, `inotify`, `timerfd`, `signalfd`,
  POSIX timers and extended attributes.
- On AArch64 the LibOS passes `AT_HWCAP` and `AT_HWCAP2` (masked to an allowlist) on every PAL.
- Compatibility is measured with the Linux Test Project (native, direct and VM loaders, x86_64 and AArch64); a
  profiler (`scripts/syscall-profile.py`) predicts from an `strace` whether an application is likely to run.

## Examples, installation and documentation

- **`./exec.sh --<backend>`** in every `CI-Examples/` directory (`direct`, `vm`, `sev`, `sev-snp`, `sgx`, `vbs`)
  explains why a backend cannot run, prints the command under test and streams the logs; each example has a check.
- **`scripts/install.sh`** installs the runtime for a backend; `container/run.sh` provides a rootless container
  per checkout. The quickstart guides use the container as the only route, except Windows VBS.
- **Documentation.** `scripts/check-docs.py` verifies the paths and links of the documents; GitHub workflows cover
  direct x86_64, direct AArch64, emulated RISC-V, VM x86_64 and SEV.

## Inherited from upstream

The manifest syntax, the TDX backend of gramine-tdx and the SGX tooling are inherited; the SGX documentation is
unchanged except where noted above.
