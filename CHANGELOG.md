# Changelog

This tree extends [gramine-tdx](https://github.com/gramineproject/gramine-tdx), the Intel TDX and
virtual-machine fork of Gramine, with additional platform adaptation layers (PALs), architectures and
tooling. This document lists what the tree adds, grouped by area. Each area states the contribution, the
design decisions that matter, the findings worth recording, how to use it and its limits. Status words follow
[README.md](README.md): *done* (works and is tested here), *experimental* (works in the tested setups, with known
gaps), *planned*. The whole tree is experimental and not for production use. Open items are in
[TODO.md](TODO.md); measured results of the examples are in [CI-Examples/RESULTS.md](CI-Examples/RESULTS.md).

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

## Summary of results

- One library OS now runs on three instruction sets (x86_64, AArch64, RISC-V) and on six isolation backends
  (process, microVM, SEV, SEV-SNP, SGX, Windows VBS enclave) with the same applications and the same check scripts.
- The LibOS regression suite passes on the x86_64 and AArch64 microVMs (168 and 167 tests) and, apart from one
  test, inside SEV-SNP guests.
- Rewriting system-call sites makes a call 3.3 to 4.1 times cheaper than a trap on the RISC-V board measured,
  which removes the need for a patched C library on AArch64 and RISC-V.
- Unmodified OpenGL games run at 76 to 102 % of native speed through the host GPU on `gramine-direct`, and on
  `gramine-vm` through a redirection to the hypervisor.
- A Linux application can run inside a Windows VBS enclave, although the enclave cannot make memory executable at
  run time, by packing the library OS, the libc and the application into the signed image at build time.

## Direct PAL on AArch64 and RISC-V

### AArch64

`gramine-direct` runs on AArch64 Linux. The LibOS regression reaches about 195 passed and 36 skipped, the PAL
regression 28 passed and 8 skipped, and twelve example suites pass.

Design decisions:
- The syscall table is indexed by the generic Linux numbers; the legacy x86-only calls are absent, as in the
  AArch64 kernel.
- A thread's `PAL_TCB` is found through a tag bit in `TPIDR_EL0` for Gramine's own threads and otherwise
  through a 128-entry registry keyed on the application's TLS pointer. An application can change `TPIDR_EL0`
  without a trap, so there is nothing to intercept.
- Application syscalls are caught by seccomp (`SIGSYS`). The resume address travels in `x16`, because `svc`
  leaves `x30` holding the application's live return address.
- A context that came from the host is resumed through the kernel's own `sigreturn`, so every register and flag is
  restored exactly; the LibOS supplies the signal trampoline because AArch64 glibc passes no restorer.

Findings:
- The trap path overwrote `x30`; the raw `clone` argument order differs from x86_64; all threads shared one
  `PAL_TCB` until the registry existed; `x0` must be zero at program entry; returning from an asynchronous
  signal clobbered `x17` and the flags.
- `AT_HWCAP` and `AT_HWCAP2` are now passed to applications on every AArch64 PAL, masked to an allowlist
  (SVE, PAC, BTI and MTE stay hidden until the LibOS supports them), so libraries select hardware AES, CRC and
  atomics.

### RISC-V

The direct PAL is cross-built for riscv64. In an emulated Ubuntu guest the PAL regression passes 27 tests (9
skipped) and the LibOS regression 144 (36 skipped); eleven example suites pass with the bundled glibc. A run on
an 8-hart board completed the basic suites; its single failure is caused by the board's memory size.

Design decisions:
- `tp` belongs to the application's libc, so the PAL finds its control block as on AArch64.
- The resume address is stored in the thread control block, not in a register, because RISC-V glibc expects a
  system call to preserve every register but `a0`.
- The return to the application builds a signal frame and calls `rt_sigreturn`, which restores all registers
  at once. `statx` is enabled for riscv64 only, because RISC-V glibc does not fall back to `newfstatat`.

Findings: a cloned thread kept its parent's `tp`; the first system-call path clobbered `t0` and `t1`; and the
guest's shared directory served stale binaries until it was mounted with `cache=none`.

### System calls without a patched libc

The first trap at an `svc` (AArch64) or `ecall` (RISC-V) site rewrites the instruction into a branch to a
per-site stub (`sys.rewrite_syscalls`). After the first call, a system call no longer traps, and no patched libc is
needed. x86_64 keeps its patched glibc.

- **Result.** On the RISC-V board a rewritten call costs 1.9 to 2.6 us against 7.8 to 8.4 us for the trap (3.3 to
  4.1 times cheaper). In the emulated guest, a stress test passes 40 of 40 runs in every scenario.
- **Design.** RISC-V stubs are reached by `jal` (about +-1 MiB). A 2-byte-aligned `ecall` is published through
  `c.ebreak`, and an instruction-cache flush makes it visible to the other harts. A "late handler" treats a trap at a
  site another thread has just rewritten as a normal system call.
- **Findings.** The first stubs saved the wrong register range, losing `t3` and `t4` across a patched call; and
  the unaligned publication protocol raced with a thread running the site.
- **Limits.** One hosted stress alarm (`test_603`, a timeout) occurred on two CI runs and not in about 90 local
  runs; the test is deselected in the hosted workflow. Larger runs under load on hardware are not done.

### Host libc

With `libc=none`, no glibc is built and applications run on the host's. It is the default for VM builds and for
the direct AArch64 and RISC-V builds; x86_64 direct and SGX keep the bundled glibc. The VM build step in CI went
from 5 min 45 s to 1 min 19 s with identical test results. `gramine.runtimedir()` finds the host directory when the
manifest is rendered.

## VM PAL (`gramine-vm`)

The VM PAL runs the library OS as a unikernel in a microVM, under stock QEMU with KVM, at 1 to 4 vCPUs.

### AArch64 port

- QEMU enters the ELF at EL1 with the MMU off. One assembly file builds an identity map and jumps to C. GICv3
  replaces the APIC, the generic virtual timer replaces the local APIC timer, and the virtio-pci drivers are
  shared with x86_64 through ECAM; secondary CPUs start through PSCI.
- LibOS regression at 2 vCPUs: 86 passed, 90 skipped, 2 expected failures, 0 failed (with TCP and UDP through the
  relay), later 167 passed.
- Findings (more than twenty were logged): the GIC CPU interface was never enabled; the syscall entry parked a
  register in `SPSR_EL1`, which the hardware overwrites, which was the cause of the multi-CPU hangs; the
  LibOS syscall path ran with interrupts masked; and the guest trapped on `mrs ctr_el0` until
  `SCTLR_EL1.UCT` was set.

### Processes

With `sys.experimental__multiprocess` the guest supports `fork`, `vfork`, `execve`, `wait`, signals between
processes, pipes, futexes shared between processes, a guest-wide `/dev/shm` and System V IPC. Shells, nginx with
workers and Redis `BGSAVE` run at 1, 2 and 4 vCPUs on x86_64 and on AArch64.

- AArch64 needed a frame pool, per-process translation roots, a root switch at context change and demand-zero
  faults. A thousand forks at 4 vCPUs take 32 to 42 s instead of up to 480 s after interruptible waits were added.
- Findings: a process's startup allocations can be freed while a child still shares them (a hang in a signals
  probe); a startup fault appeared only at some install-path lengths, for example `~/.local/gramine`, and is
  covered by a prefix-sweep test; and a timer interrupt arriving before the IDT is installed caused a boot flake
  under host load in CI.
- AArch64 FP/SIMD state at system-call return, and the application's `x16`, had to be preserved across `svc` and
  `rt_sigreturn`.

### Networking

Guest sockets are carried over vsock to one relay process per VM: TCP, UDP and DNS, loopback, non-blocking
`connect` and `SO_RCVTIMEO`. The manifest's `vm.net` table sets the outbound policy and the inbound forwards.
The redis, memcached, lighttpd, nginx and rust examples run through the relay on both architectures.

- The relay reaches about 12 % (AArch64) to 36 % (x86_64) of native Redis throughput.
- Findings: a data packet submitted without notifying the host stalled `memcached`; a guest without a vsock device
  crashed; on a fast host the guest dropped packets when the byte credit outlasted the ring slots, which made
  `mongodb` fail under `vm`, `sev` and `sev-snp`; the fix merges payloads into the newest queued packet.
- Silencing the relay's `listening` line made runs about five seconds slower; the cause was not established, and
  the line is kept.

### Timers

The one-shot timer of CPU 0 is armed for the earliest pending timeout and the 100 ms tick stays the ceiling; a
registration from another vCPU sends CPU 0 a timer IPI. Short timed waits take about their length instead of
100 ms (x86_64, AArch64 and SEV-SNP). Pipe round trips at 4 vCPUs on AArch64 remain slow.

### Use and limits

`./exec.sh --vm` in an example directory; guide: [quickstart/vm.md](Documentation/quickstart/vm.md).
All examples pass on x86_64. On AArch64, `gcc`, `openjdk`, `pytorch` and `python` fail because the guest cannot
map the distribution's program. Hosted AArch64 runners have no KVM, so the AArch64 VM is not in CI.

## AMD SEV and SEV-SNP (`gramine-sev`)

The VM PAL runs as a SEV or SEV-SNP guest. The guest kernel is launched from an IGVM file whose launch
measurement covers the kernel, so the measurement can be recomputed from the file.

- **Result.** The LibOS suite passes under SEV-SNP at 1, 2 and 4 vCPUs, except `test_020_cpuinfo`: the host
  reports `mwaitx`, which the KVM guest does not. All examples pass in both modes.
- **Attestation.** A guest requests a hardware-signed report through `/dev/attestation`; the report verifies
  against AMD's VCEK certificate chain and its measurement equals the digest computed from the IGVM file.
- **Design.** The VM PAL itself is unchanged by this work (only assert line numbers changed). A patched QEMU
  (version 11.1.2, built by `container/sev/`) adds the IGVM measurement support.
- **Modes.** `./exec.sh --sev` tests plain SEV and `--sev-snp` tests SEV-SNP as separate runs.
- **Use.** [quickstart/sev.md](Documentation/quickstart/sev.md).
- **Limits.** Trusted files and Secure TSC are not implemented. The tested host is a single AMD EPYC 7443P
  system.

## Intel SGX

The inherited SGX backend is built and tested again. `scripts/install.sh --sgx` installs it, and
`container/run.sh` passes the enclave devices and the AESM socket into the rootless container.

- **Result.** 19 of the 20 examples pass under `./exec.sh --sgx`. `unit_tests-mremap` fails because a shrunk
  mapping remains readable. The hardware is a Xeon Gold 6430 (SGX1 and SGX2, flexible launch control).
- **Finding.** Enclave creation failed with `EPERM` in the rootless container. The apparent cause was
  `vm.mmap_min_addr`, but the actual cause was that a rootless `--device` mount is `noexec`, so the enclave's
  `PROT_EXEC` mapping was refused. Binding the device nodes with `exec` fixes it.
- Several manifests had to be corrected for SGX (`helloworld`, `gcc`, `openjdk`, `blender`).
- **Use.** [quickstart/tdx-sgx.md](Documentation/quickstart/tdx-sgx.md).
- **Limits.** Remote attestation and the `ra-tls` examples are not covered.

## Windows VBS enclaves (`gramine-vbs`)

The `vbs` PAL runs an unmodified Linux application inside a virtualization-based security enclave on Windows 11
(ARM64 and x64). A VBS enclave cannot make memory executable at run time, so the library OS, a glibc and the
application are packed into a signed enclave image at build time. The tool builds from WSL.

- **Supported.** Threads, signals, host files, sockets, Python, servers (Redis, NGINX, memcached, lighttpd),
  SQLite, curl, a Rust server, `iperf3` and BusyBox applets, with a local attestation report and verifier. An
  opt-in TLS example binds its key to the report.
- **Validation.** On ARM64, 11 of 12 example directories and all 8 static VBS examples pass. The LibOS regression
  subset passes 71 of 102 cases; every failure is classified (`fork`, platform limits).
- **Performance.** An enclave call costs about 7 us. Counting host calls and removing staging buffers improved
  throughput 1.8 times for Redis, 1.6 times for memcached and 5 to 7 times for NGINX with 1 MiB bodies.
- **Platform findings.** The enclave environment does not preserve `TPIDR_EL0`, resets `x18` on preemption, gives
  threads an 8 KiB stack and hangs on some faults. `nodejs` fails for the same reason: `libstdc++` uses the
  reserved register `x18`. A strict memory policy also failed on one configuration.
- **Intermittent failure.** The `helloworld` check fails now and then right after its image is built and signed
  (`mmap` of a 512 MiB region): once in six rebuild-and-check cycles, never in thirteen checks of a used image.
- **Use.** `scripts/install.sh --vbs`, then `./exec.sh --vbs`;
  [quickstart/vbs-windows.md](Documentation/quickstart/vbs-windows.md) and [vbs.md](Documentation/vbs.md).
- **Limits.** No `fork`, no run-time code generation (so no JIT), test-signed debug enclaves only.

## Graphics

### Host GPU for `gramine-direct`

Opt-in manifest keys let an unmodified OpenGL application (and, on x86_64, a Vulkan application) use the host's
render node and X server. `glxgears`, Quake II, SuperTuxKart and Minetest reach 97 to 102 % of native speed on
Intel, 76 to 91 % on AMD, and 88 to 100 % on an NVIDIA GB10 with the proprietary driver (AArch64).

- **Findings.** The NVIDIA driver needed file-descriptor translation for its ioctls; the X connection needed
  socketpair ends passed across the boundary; and a `poll` and an `mremap` defect surfaced only with a real driver.
- **Limits.** `gramine-direct` is not a sandbox and the keys grant the application access to the GPU. Sound and
  several GPU nodes are not supported.

### Redirected graphics for `gramine-vm`

EGL, GBM and DRM calls of the guest are forwarded through a fixed shared page to a listener in a QEMU fork that
uses the host GPU with stock Mesa. `glxgears`, SuperTuxKart, Quake II and Minetest run (x86_64 only). Vulkan is
provided by an implicit layer in `libgraphics.so`; Quake II's Vulkan renderer reaches 86 % of native speed on
Intel and 72 % on AMD.

- **Finding.** Computing the guest memory layout at launch, instead of fixing it, reduced boot at 8 GB from 2.9 s
  to 0.8 s, and host memory follows what the guest touches.
- **Limits.** The listener and the shared-page address are x86-only, which blocks AArch64. Minetest reaches 25 to
  35 % of native speed.

### Windows games

Under `gramine-vm`, GE-Proton's Wine runs `hello.exe` and DOOM I Enhanced (64-bit, Direct3D 11 through DXVK) to its
first level, with keyboard input, in software rendering (44 to 48 frames per second). It required named Unix-socket
nodes, `fork` without copying reservations, `SCM_RIGHTS`, a virtual `%gs`, exception events, a 512 GiB address space
and syscall user dispatch. This is a feasibility result: Wine's GPU path and 32-bit games are open.

## LibOS system calls and compatibility

System calls were added in rounds, each driven by the Linux Test Project: `mremap`, System V semaphores, shared
memory and message queues, `faccessat2`, `getresuid`, more `prctl` options, file times and links, vectored I/O,
`memfd` with seals, `inotify`, `timerfd`, `signalfd`, POSIX timers and extended attributes.

- **Result.** LTP passes under the direct loader rose from 620 to 739 and under the VM without `fork` from 490 to
  596 (unmodified VM: 315 to 380).
- **Matrices.** 1,327 LTP scenarios are measured natively, under `gramine-direct` and under `gramine-vm`, on x86_64
  and AArch64, with a reason for every non-pass. AArch64 passes: native 944, direct 611, VM unmodified 315, VM
  no-fork 482 (x86_64: 955, 620, 315, 490). With the no-fork shim on x86_64, 477 pass, 185 fail, 250 are
  not applicable, 390 break, 23 time out and 2 panic.
- **Finding.** Most differences between architectures come from tests that do not exist on AArch64 and from the
  host kernel; a few are AArch64 defects, for example in `preadv2` and `pwritev2`, which are fixed.
- **Profiler.** `scripts/syscall-profile.py` traces an application with `strace` and predicts whether the direct and
  VM loaders support its system calls, with a verdict of likely, may run or unlikely. It matches the observed result
  for 20 of 20 cases on ten applications; the test is weak, since the sample is small.

## Examples, installation and documentation

- **`./exec.sh --<backend>`** in every `CI-Examples/` directory (`direct`, `vm`, `sev`, `sev-snp`, `sgx`, `vbs`)
  explains why a backend cannot run, prints the command under test and streams the logs; each example has a check.
  Shared helpers handle server examples, so a hung server cannot block a run.
- **`scripts/install.sh`** installs the runtime for a backend, and `container/run.sh` provides a rootless
  container per checkout. The quickstart guides use the container as the only route, except Windows VBS.
- **Findings from running everything.** Writing the guides on real machines found an x86_64 LibOS regression with the
  bundled glibc and that the `tdx` PAL does not build. A direct run of the iperf example
  hung for 600 s until the shared server helpers started servers in their own process group.
- **Documentation.** `scripts/check-docs.py` verifies the paths and links of the documents. Workflows cover direct
  x86_64, direct AArch64, emulated RISC-V, VM x86_64 and SEV.

## Inherited from upstream

The manifest syntax, the TDX backend of gramine-tdx and the SGX tooling are inherited; the SGX documentation is
unchanged except where noted above.
