# Features

What Saguaro adds to [gramine-tdx](https://github.com/gramineproject/gramine-tdx) (Intel's Gramine
with TDX and virtual-machine backends). One or two lines each; the plan, log,
open items and test results of every feature are in the development notes (a private repository),
under the topic named at its end (`dev docs: <topic>`).

Status words: **done** (works and is tested here), **experimental** (works in the tested setups, with
known gaps). The whole tree is experimental and not for
production use.

## Added so far

### Platforms

- **AArch64 direct PAL** (done). `saguaro-direct` runs on AArch64 Linux: LibOS regression about 195 passed / 36
  skipped, PAL regression 28 / 8, 12 example suites. `dev docs: aarch64/direct/`
- **AArch64 vm PAL** (experimental). `saguaro-vm` boots a microVM under stock QEMU `virt` with KVM at 1 to 4
  vCPUs and runs LibOS applications, by default with several processes (`sys.experimental__multiprocess`):
  `fork`, `vfork`, `execve`, pipes, signals and shared memory between processes (bash, nginx workers, redis
  `BGSAVE`). [CHANGELOG.md](../CHANGELOG.md), `dev docs: vm-multiproc-aarch64/`
- **RISC-V (RV64) direct PAL** (experimental). Cross-built; the regression suites and 11 example suites pass in an
  emulated RISC-V guest and, natively, on a board (PAL 27 passed, LibOS 190 passed and 3 failed, all 12 example
  suites). `dev docs: riscv/`
- **x86_64 vm PAL, extended** (experimental). The inherited `saguaro-vm` unikernel got a reworked vsock driver,
  PCI/ivshmem support and uncacheable shared mappings; it is the base of the graphics and network work.
  `dev docs: vm.md`

- **AMD SEV and SEV-SNP** (experimental, AMD64). `saguaro-sev` runs the VM PAL as a SEV or SEV-SNP guest launched from an
  IGVM file; the launch measurement covers the guest kernel and can be recomputed from the file. Attestation reports
  from `/dev/attestation` verify against AMD's VCEK chain. The LibOS suite passes except `test_020_cpuinfo` (the host
  has `mwaitx`, the KVM guest does not), and all examples pass in both modes. Trusted files and Secure TSC are
  open. [quickstart/sev.md](quickstart/sev.md), `dev docs: sev/`
- **Intel SGX route** (experimental, Intel x86_64). The inherited SGX backend builds and runs again: `scripts/install.sh --sgx`
  and a rootless container that passes the enclave devices (bound with `exec`, which a rootless `--device` mount
  lacks). 19 of 20 examples pass under `./exec.sh --sgx`. Remote attestation and the `ra-tls` examples are not
  covered. [quickstart/sgx.md](quickstart/sgx.md)
- **Windows VBS enclaves** (experimental, ARM64 and x64 Windows 11). The `vbs` PAL runs an application inside a
  signed VBS enclave: the library OS, a glibc and the application are packed into the enclave image at build
  time. Threads, signals, host files and sockets, Python, servers (Redis, NGINX, memcached, lighttpd), SQLite,
  curl (HTTP), a Rust server, `iperf3` and single BusyBox applets run, with local attestation; an opt-in TLS
  example binds its key to the attestation report. `nodejs`, NumPy/SciPy, MongoDB and PyTorch are blocked by the
  ARM64 `x18` rule or the loader, `openjdk` by run-time code generation and R by process creation
  (`dev docs: vbs-enclave/APPS.md`). Limits: no `fork`, no run-time code loading (no JIT), test-signed debug enclaves.
  `saguaro-vbs`, `scripts/install.sh --vbs` and `./exec.sh --vbs` drive it. On both ARM64 and x64, 11 of 12 examples and
  all 8 static examples pass (`nodejs` fails: the x18 rule on ARM64, no x64 recipe on x64).
  `dev docs: vbs-enclave/manual.md`

### System calls and libc

- **Runtime syscall-site rewriting** (done on AArch64 and RISC-V). The first trap at an `svc` or `ecall` site
  rewrites it into a branch to a stub (`sys.rewrite_syscalls`, on by default), so no patched libc is needed; on
  RISC-V hardware a syscall is 3.3 to 4.1 times cheaper than the trap. [CHANGELOG.md](../CHANGELOG.md)
- **Host libc instead of a built glibc** (done). With `libc=none` no glibc is built and applications run on the
  host's: the default for VM builds and for direct AArch64 and RISC-V builds; x86_64 direct keeps the patched
  glibc. The VM build in CI went from 5m45s to 1m19s. `dev docs: runtime-libc/`
- **More LibOS system calls** (done). `mremap`, SysV semaphores, `faccessat2`, `getresuid`/`getresgid`, more
  `prctl` options, and `statx` on RISC-V (`dev docs: graphics/tirith-delta.md`); then, in two rounds, file times and links,
  uid and capabilities, vectored I/O, `memfd` with seals, SysV shared memory and message queues, `inotify`,
  `timerfd`, `signalfd` and POSIX timers, and extended attributes. `dev docs: libos-syscalls/`
- **CPU feature words on AArch64** (done). The LibOS passes `AT_HWCAP` and `AT_HWCAP2` to applications on every
  AArch64 PAL, masked to an allowlist, so libraries pick hardware AES, CRC and atomics. `dev docs: aarch64/hwcap/`
- **VM syscall compatibility matrix** (measured on x86_64 and AArch64). LTP results for native, direct and VM
  loaders, with per-test reasons and rerun instructions; the AArch64 runs are sharded over parallel runners
  (`scripts/ltp-sharded.py`). `dev docs: syscall-compat/MATRIX.md`, `dev docs: syscall-compat/MATRIX-aarch64.md`
- **Application syscall profile** (experimental). `scripts/syscall-profile.py` traces an application with
  `strace` and reports which of its syscalls the direct and vm loaders support, with a likely/may run/unlikely
  verdict; calibrated on ten applications. `dev docs: syscall-profile/`

### The VM PAL (`saguaro-vm`)

- **Processes** (experimental, x86_64 and AArch64). `fork`, `vfork`, `execve`, `wait`, signals between processes,
  pipes, shared futexes, guest-wide `/dev/shm` and System V IPC, on by default on both architectures
  (`sys.experimental__multiprocess = false` turns them off); shells, nginx with workers and Redis `BGSAVE` run.
  On AArch64 the same works at 1, 2 and 4 vCPUs (frame pool, per-process
  translation roots, interruptible waits, `_PalThreadResume`): Bash, nginx with four workers and Redis `BGSAVE` run,
  a thousand forks at 4 vCPUs take 32 to 42 s instead of up to 480 s, and an LTP run with the key on passes 720 of
  1,327 scenarios without a guest panic.
  `dev docs: vm-multiproc/`, `dev docs: vm-multiproc-aarch64/`

- **Timer granularity** (done, x86_64, AArch64 and SEV-SNP). CPU 0's one-shot timer is armed for the earliest pending
  timeout and the 100 ms tick stays the ceiling, so short timed waits take about their length. `dev docs: vm-timer/`

- **Windows games with Wine and Proton** (experimental, x86_64 `vm`). GE-Proton's Wine runs in a guest: `hello.exe`,
  then DOOM I Enhanced (64-bit, Direct3D 11 through DXVK) to its title screen and first level with keyboard input,
  in software rendering (44 to 48 FPS, about 57 % of native at equal thread counts). It needed named Unix-socket
  nodes, `fork` without copying reservations, `SCM_RIGHTS`, a virtual `%gs` for the application, interruptible waits,
  exception events, a 512 GiB address space, syscall user dispatch and a loopback netlink socket. Native Vulkan
  programs reach the GPU from demand-paged processes; Wine's GPU path and 32-bit games (Psychonauts) are open.
  `dev docs: proton-vm/`

- **TCP and UDP in the guest** (experimental). Guest sockets are carried over vsock to one relay process per
  VM: TCP, UDP, DNS, loopback, non-blocking connect, `SO_RCVTIMEO`, with outbound policy and inbound forwards
  from the manifest's `vm.net`; x86_64 and AArch64. [CHANGELOG.md](../CHANGELOG.md)
- **Graphics host redirection** (experimental, x86_64 only). EGL, GBM and DRM calls of the application are
  forwarded through a shared page to a listener in QEMU that uses the host GPU, with stock Mesa; `glxgears`,
  SuperTuxKart, Quake II and Minetest run. `dev docs: graphics/`
- **Vulkan** (experimental, x86_64). Works under `saguaro-direct` as is; under `saguaro-vm`, an implicit layer in
  `libgraphics.so` provides the device path and presentation (Quake II's Vulkan renderer runs). `dev docs: vulkan/`
- **Lower graphics memory floor** (experimental). The guest memory layout is computed at launch: boot takes
  about 0.8 s at 8G instead of 2.9 s, and host memory follows what the guest touches. `dev docs: graphics/`

### `saguaro-direct` with the host GPU

- **Accelerated graphics** (experimental). Opt-in manifest keys let an unmodified OpenGL application (and, on
  x86_64, a Vulkan application) use the host's render node and X server: `glxgears`, Quake II, SuperTuxKart and
  Minetest at 76 to 102 % of native speed on Intel and AMD GPUs (x86_64), and at 88 to 100 % on an NVIDIA GPU
  with the proprietary driver (AArch64). `saguaro-direct` is not a sandbox, and the keys give the application
  access to the GPU. `dev docs: graphics-direct/`, `dev docs: graphics-aarch64/`

### Build, test and workflow

- **`scripts/install.sh`** (done). One command builds and installs the direct and VM PALs (`--vm`,
  `--graphics`, `--libc`, `--target riscv64`) and the `--sev`, `--sgx` and `--vbs` runtimes, and can run the tests. [INSTALL.md](../INSTALL.md)
- **Containers and workspaces** (done). `container/run.sh` gives each checkout its own rootless container,
  guest CIDs and ports, so several branches and agents work side by side (the header of `container/run.sh`).
- **`./exec.sh --<backend>`** (done). Every `CI-Examples/` directory runs with `--direct`, `--vm`, `--sev`, `--sev-snp`,
  `--sgx` or `--vbs`: it explains why a backend cannot run, prints the command under test, streams the logs and ends
  with the example's check. The shared helpers (`CI-Examples/common_tools/`) also start and stop the server examples.
  Results per backend: [CI-Examples/README.md](../CI-Examples/README.md), [CI-Examples/RESULTS.md](../CI-Examples/RESULTS.md).
- **Test batteries** (done). `test/quick.sh <variant>` runs three tiers (smoke, regression, extended) with one
  results format; the regression tier is the PAL suite, a core list of 127 LibOS tests, nine examples through their
  `exec.sh` and the steps of the variant, at 2 vCPUs on the VM variants (`test/README.md`). Underneath:
  `test/direct/test-direct.sh` (PAL, LibOS), `test/vm/test-vm.sh`, `test/sev/test-sev.sh`, the VM stress tests
  (`test/vm/stress/`) and the VM skip list.
  `dev docs: multiarch.md`
- **CI workflows** (done). Manual workflows for direct x86_64, direct AArch64, emulated RISC-V, the x86_64
  VM PAL, the AArch64 VM PAL under software emulation (TCG), and the documentation check under `.github/workflows/`. `dev docs: multiarch.md`
- **RISC-V test guest** (done). `scripts/riscv64/` boots an Ubuntu guest in QEMU for the cross-built tests.
  `dev docs: riscv/`
- **Quickstart guides** (done). One guide per platform; the container is the only route except Windows VBS, and
  `./exec.sh` runs the examples. [quickstart/README.md](quickstart/README.md)
- **Brief public documents** (done). `CHANGELOG.md` (what the tree adds, by area, at the top of the repository) and `TODO.md` (open items, in this folder). The plans,
  findings and test records of every topic are in a private repository.

### Inherited

- The Intel TDX and VM backends of gramine-tdx, and the SGX backend (see above). The examples that run on SEV
  also run on TDX; `exec.sh` has no TDX backend. CI compiles the `tdx` PAL.
