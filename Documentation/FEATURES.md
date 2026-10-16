# Features

What this tree adds to [gramine-tdx](https://github.com/gramineproject/gramine-tdx) (Intel's Gramine
with TDX and virtual-machine backends), and what is planned. One or two lines each; the plan, log,
open items and test results of every feature are in the directory named at its end, under
`Documentation/devel/`. `devel/summary.md` gives the outcome of each on one page.

Status words: **done** (works and is tested here), **experimental** (works in the tested setups, with
known gaps), **planned** (not started or not usable yet). The whole tree is experimental and not for
production use.

## Added so far

### Platforms

- **AArch64 direct PAL** (done). `gramine-direct` runs on AArch64 Linux: LibOS regression 145 passed / 36
  skipped, PAL regression 28 / 8, 12 example suites. `aarch64/direct/`
- **AArch64 vm PAL** (experimental). `gramine-vm` boots a microVM under stock QEMU `virt` with KVM at 1 to 4
  vCPUs and runs LibOS applications; `fork`, `vfork` and `execve` are available behind
  `sys.experimental__multiprocess`. `aarch64/vm/`, `vm-multiproc-aarch64/`
- **RISC-V (RV64) direct PAL** (experimental). Cross-built; the regression suites and 11 example suites pass in an
  emulated RISC-V guest, not yet on hardware. `riscv/`
- **x86_64 vm PAL, extended** (experimental). The inherited `gramine-vm` unikernel got a reworked vsock driver,
  PCI/ivshmem support and uncacheable shared mappings; it is the base of the graphics and network work.
  `devel/vm.md`

- **Windows VBS enclaves** (experimental, ARM64 and x64 Windows 11). The `vbs` PAL runs an application inside a
  signed VBS enclave: the library OS, a glibc and the application are packed into the enclave image at build
  time; threads, signals, host files and sockets, Python and servers (Redis, NGINX, memcached, lighttpd) run, with
  local attestation. Limits: no `fork`, no run-time code loading (no JIT), test-signed debug enclaves.
  `gramine-vbs` and `make VBS=1` drive it. `Documentation/vbs.rst`, `vbs-enclave/`

### System calls and libc

- **Runtime syscall-site rewriting** (done on AArch64 and RISC-V). The first trap at an `svc` or `ecall` site
  rewrites it into a branch to a stub (`sys.rewrite_syscalls`, on by default), so no patched libc is needed; on a
  RISC-V board (Ky X1) a syscall is 3.3 to 4.1 times cheaper than the trap. `syscall-interception/`,
  `riscv-hardware/`
- **Host libc instead of a built glibc** (done). With `libc=none` no glibc is built and applications run on the
  host's: the default for VM builds and for direct AArch64 and RISC-V builds; x86_64 direct keeps the patched
  glibc. The VM build in CI went from 5m45s to 1m19s. `runtime-libc/`
- **More LibOS system calls** (done). `mremap`, SysV semaphores, `faccessat2`, `getresuid`/`getresgid`, more
  `prctl` options, and `statx` on RISC-V (`graphics/tirith-delta.md`); then, in two rounds, file times and links,
  uid and capabilities, vectored I/O, `memfd` with seals, SysV shared memory and message queues, `inotify`,
  `timerfd`, `signalfd` and POSIX timers, and extended attributes. `libos-syscalls/`
- **CPU feature words on AArch64** (done). The LibOS passes `AT_HWCAP` and `AT_HWCAP2` to applications on every
  AArch64 PAL, masked to an allowlist, so libraries pick hardware AES, CRC and atomics. `aarch64/hwcap/`
- **VM syscall compatibility matrix** (measured on x86_64 and AArch64). LTP results for native, direct and VM
  loaders, with per-test reasons and rerun instructions; the AArch64 runs are sharded over parallel runners
  (`scripts/ltp-sharded.py`). `devel/syscall-compat/MATRIX.md`, `devel/syscall-compat/MATRIX-aarch64.md`
- **Application syscall profile** (experimental). `scripts/syscall-profile.py` traces an application with
  `strace` and reports which of its syscalls the direct and vm loaders support, with a likely/may run/unlikely
  verdict; calibrated on ten applications. `devel/syscall-profile/`

### The VM PAL (`gramine-vm`)

- **Processes** (experimental, x86_64 and AArch64). `fork`, `vfork`, `execve`, `wait`, signals between processes,
  pipes, shared futexes, guest-wide `/dev/shm` and System V IPC, behind `sys.experimental__multiprocess`; shells,
  nginx with workers and Redis `BGSAVE` run. `vm-multiproc/`, `vm-multiproc-aarch64/`

- **TCP and UDP in the guest** (experimental). Guest sockets are carried over vsock to one relay process per
  VM: TCP, UDP, DNS, loopback, non-blocking connect, `SO_RCVTIMEO`, with outbound policy and inbound forwards
  from the manifest's `vm.net`; x86_64 and AArch64. `vm-network/`
- **Graphics host redirection** (experimental, x86_64 only). EGL, GBM and DRM calls of the application are
  forwarded through a shared page to a listener in QEMU that uses the host GPU, with stock Mesa; `glxgears`,
  SuperTuxKart, Quake II and Minetest run. `graphics/`
- **Vulkan** (experimental, x86_64). Works under `gramine-direct` as is; under `gramine-vm`, an implicit layer in
  `libgraphics.so` provides the device path and presentation (Quake II's Vulkan renderer runs). `vulkan/`
- **Lower graphics memory floor** (experimental). The guest memory layout is computed at launch: boot takes
  about 0.8 s at 8G instead of 2.9 s, and host memory follows what the guest touches. `graphics/`

### `gramine-direct` with the host GPU

- **Accelerated graphics** (experimental). Opt-in manifest keys let an unmodified OpenGL application (and, on
  x86_64, a Vulkan application) use the host's render node and X server: `glxgears`, Quake II, SuperTuxKart and
  Minetest at 76 to 102 % of native speed on Intel and AMD GPUs (x86_64), and at 88 to 100 % on NVIDIA's GB10
  with the proprietary driver (AArch64). `gramine-direct` is not a sandbox, and the keys give the application
  access to the GPU. `graphics-direct/`, `graphics-aarch64/`

### Build, test and workflow

- **`scripts/install.sh`** (done). One command builds and installs the direct and VM PALs (`--vm`,
  `--graphics`, `--libc`, `--target riscv64`) and can run the tests. `INSTALL.rst`
- **Containers and workspaces** (done). `container/run.sh` gives each checkout its own rootless container,
  guest CIDs and ports, so several branches and agents work side by side. `AGENTS.md`
- **Test batteries** (done). `test/test-direct.sh` (PAL, LibOS, 12 example suites; `GRAMINE_RUNNER=gramine-vm`
  runs the server examples under the VM), the `test/test-vm*.sh` scripts and the VM skip list.
  `devel/multiarch.md`
- **CI workflows** (done). Manual workflows for direct x86_64, direct AArch64, emulated RISC-V and the x86_64
  VM PAL under `.github/workflows/`. `devel/multiarch.md`
- **RISC-V test guest** (done). `scripts/riscv64/` boots an Ubuntu guest in QEMU for the cross-built tests.
  `riscv/`
- **Quickstart guides** (done). One guide per platform, with the commands run on a machine of that platform.
  `Documentation/quickstart/`
- **Development documents** (done). Plan, findings, open items and test results per topic. `devel/README.md`

### Inherited

- The Intel TDX and VM backends of gramine-tdx; the SGX backend is built but not tested here. The `tdx` PAL is
  compiled in CI only.

## Planned

- **VM memory size from the manifest.** The guest RAM of `gramine-vm` chosen by a manifest key instead of a
  launcher default (proposed, branch `dev-vm-memory`).
- **RISC-V on hardware, and a RISC-V vm PAL.** Validate the direct PAL and the rewriting on a board, then make
  rewriting the default; the vm PAL is not started.
- **Graphics in the vm PAL on AArch64.** Deferred until the QEMU listener is redesigned. `graphics-aarch64/`
- **Graphics fixes.** Faster Quake II mouse look and Minetest, a discrete GPU, Team Fortress 2, huge pages, more
  vCPUs. `graphics/TODO.md`
- **Proton (Wine, DXVK, VKD3D-Proton) under `gramine-vm`.** In progress: `hello.exe` runs in a guest; games and
  Direct3D are next. `proton-vm/`
- **AMD SEV.** A confidential-VM backend for AMD64; no code yet.
- **VBS: more.** `fork`, sealing, production signing, a Windows-only build without WSL. `vbs-enclave/TODO.md`
- **Asynchronous signals and a finer timer in the vm PAL.** `_PalThreadResume()` is a stub, so a signal does not
  end a thread's blocking system call everywhere (some waits are interruptible now); timed waits are 100 ms
  ticks. `aarch64/vm/TODO.md`
- **VM networking.** IPv6 destinations, reaching guest servers from the real host, a faster relay, UDP policy,
  `SO_SNDTIMEO`. `vm-network/TODO.md`
- **Syscall interception.** An x86_64 evaluation (it keeps the patched glibc for now), one shared rewriting
  implementation for AArch64 and RISC-V, hardening of the stub pages. `syscall-interception/TODO.md`
- **Host libc.** Another distribution (Debian, Fedora), the debugger's library listing, TDX.
  `runtime-libc/TODO.md`
- **Enhanced system call support** such as [uFork](https://arxiv.org/pdf/2509.09439).
- **Cleanup for upstreaming.** One graphics module with a hook table instead of the fd-based special cases.
  `graphics/tirith-delta.md`
