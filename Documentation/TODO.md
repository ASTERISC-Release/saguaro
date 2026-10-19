# TODO

Open items are kept per topic, in the `TODO.md` of each directory under `Documentation/devel/`
(the table at the end lists them); those of finished topics, whose directory was removed, are in the section
"Open items from finished topics" below, tagged with the topic. This page is the **map by architecture and host**: for each
combination it names the main open items and points to the topic list with the details. It is kept
short on purpose; an item that is fixed is removed from the topic list first and then from here.
`FEATURES.md` lists the planned features in one line each.

## x86_64

| Host / PAL | Main open items | Details |
|---|---|---|
| `direct` | audit PAL state that a starting child allocates and frees at exit (the shared file cache was one case); sound and the remaining GPU-driver gaps for graphics | `devel/vm-multiproc/TODO.md`, `devel/graphics-direct/TODO.md` |
| `vm` | multi-vCPU performance: check and improve `gramine-vm` at 2 and 4 vCPUs (multi-process fork cost, TLB shootdowns, scheduler lock contention, cross-CPU handoffs); robust mutex for the System V shared tables; `/proc/<pid>/stat` of other processes; `PROT_NONE` enforcement; finer timer than the 100 ms tick; async signal resume; UDP/IPv6 relay gaps | `devel/vm-multiproc/TODO.md`, `devel/libos-syscalls/TODO.md`, `devel/syscall-compat/TODO.md`, and below (VM networking) |
| `vm` with graphics (OpenGL, Vulkan) | present events and a non-flipping blit for Vulkan, idle CPU of the listener, two-thread submission cost, SuperTuxKart start hang, sound, Team Fortress 2 | `devel/graphics/TODO.md`, `devel/vulkan/TODO.md` |
| `tdx` and `sgx` | never run on this tree's hardware; extend the interrupt-before-IDT guard to TDX; the SGX build needs `protobuf-c` (not in `dependencies.sh` or the container); the TDX install needs `td-shim` | `devel/vm-multiproc/TODO.md`, `quickstart/tdx-sgx.md` |
| `sev` (AMD64) | planned; no code yet | `README.md` |
| `vbs` (x64 Windows 11) | see the VBS section below | `devel/vbs-enclave/TODO.md` |
| Proton (Wine, DXVK) | phase 1 steps 1 to 3 done 2026-10-15 on `dev-proton-vm-p1` (Unix-socket nodes, fork of reservations, `SCM_RIGHTS`); `dev-proton-vm-p2` (merged, 2026-10-15): `%gs` handed to the application, interruptible waits, exception vectors, the address space to 512 GiB, drive links, syscall user dispatch: `hello.exe` prints all its lines in the VM; next: the end of a run (the wineserver stays), then the graphics tests | `devel/proton-vm/PLAN.md` |

## AArch64

| Host / PAL | Main open items | Details |
|---|---|---|
| `direct` | `kill11` hang; longer soak runs and real applications (openssh, dropbear); ABI-conformance tests | `devel/syscall-compat/TODO.md`, `devel/aarch64/direct/TODO.md` |
| `vm` | multi-process (phases 0 to 3 merged; phases 4 and 5 on `dev-aarch64-multiproc-fp`: interruptible PAL waits, EL1 LibOS faults end the process instead of the guest, 4-vCPU forks as fast as 2; the AArch64 FP/SIMD-at-syscall-return and full-sigaltstack bugs fixed 2026-10-16; phase 5 qualification done 2026-10-16 (`devel/vm-multiproc-aarch64/MATRIX-MULTIPROCESS.md`), merge pending; open: the pool's identity range; `devel/vm-multiproc-aarch64/`); console and socket waits not interruptible yet; timer granularity; `dev:`/`/sys`/attestation streams; 2 MiB pages | below (AArch64 and the VM PAL), `devel/syscall-compat/TODO.md`, `devel/vm-multiproc-aarch64/TODO.md` |
| `direct` with graphics (NVIDIA proprietary driver) | remaining NVIDIA fd-bearing ioctls; several GPU nodes; Vulkan and CUDA untested; `SCM_RIGHTS` on plain socketpairs; syscall cost of Minetest | `devel/graphics-aarch64/TODO.md` |
| `vm` with graphics | not available: the QEMU fork's listener and the comm-page address are x86-only; redesign deferred (`virtio-gpu` as one option) | `devel/graphics-aarch64/PLAN.md` |
| all AArch64 PALs: CPU feature words | `AT_HWCAP`/`AT_HWCAP2` passed (allowlist); SVE, PAC, BTI, MTE stay masked until the LibOS supports them; EL0 ID-register reads not emulated under vm; VBS features without a `PF_ARM_*` number not advertised | `devel/aarch64/hwcap/TODO.md` |

## RISC-V (RV64)

| Host / PAL | Main open items | Details |
|---|---|---|
| `direct` | validation on hardware (stress under 8-hart load, other example suites); syscall-rewriting stress and the hosted `test_603` timeout; CPU identification in the PAL | `devel/riscv/TODO.md` and below (RISC-V rewriting and hardware) |
| `vm` | not available (planned) | `README.md` |

## Windows 11 (ARM64 and x64): VBS enclaves (`vbs` PAL)

| Item | Details |
|---|---|
| **Evaluate the three VBS PAL changes in more detail** (counter-based clock, `CTR_EL0` emulation, skipping the clear of never-used pool memory); the maintainer's first look found them fine | `devel/vbs-enclave/TODO.md` ("Review and x86_64") |
| **VBS on x64 Windows 11** is done through phase 3 (2026-10-15: gate, tests, tools, six application examples; regression subset 72/103, the same failures as ARM64); left: x86_64 variants of the static `CI-Examples/vbs` manifests, the patched-glibc fast path for system calls, a CI build job, a review | `devel/vbs-enclave/PLAN-x64.md`, `devel/vbs-enclave/TODO.md` ("Review and x86_64") |
| Application examples written twice (ARM64 files on `main`, x64 files on `dev-vbs-apps2-x64`): reconcile into one architecture-neutral recipe per application; check PyTorch's image with the new `pack.py`; OpenSSL for TLS in the sysroot; AMD CPUs untested | `devel/vbs-enclave/TODO.md` |
| `fork` (a multi-enclave design), sealing, remote attestation (Azure Attestation, RA-TLS), production signing | `devel/vbs-enclave/TODO.md` |
| Networking: fewer host calls per request, TLS inside the enclave bound to the attestation report | `devel/vbs-enclave/TODO.md` ("Networking") |
| A pure-Windows build (no WSL) and a prebuilt sysroot bundle as a GitHub release; verification of the fresh-machine guide with a second WSL distribution | `devel/vbs-enclave/PLAN-windows-build.md`, `devel/vbs-enclave/windows-fresh-machine.md` |
| Platform issues found (for the maintainer to report): `TPIDR_EL0` not preserved, x18 reset on preemption, 8 KiB enclave thread stack, hanging faults, strict-memory policy failure | `devel/vbs-enclave/platform-issues.md` |
| Phase 5: threat model, boundary review, CI for what runs without VBS | `devel/vbs-enclave/PLAN.md` |

## All architectures (LibOS, tooling, documentation, CI)

- LibOS system calls still unimplemented or only partly implemented, by group: `devel/libos-syscalls/TODO.md`; the
  LTP matrix and its open failures: `devel/syscall-compat/TODO.md`; the application profiler:
  `devel/syscall-profile/TODO.md`.
- Documentation: the quickstart guides (`devel/quickstart/TODO.md`), the documentation refactor
  (`devel/docs-refactor/TODO.md`).
- CI: the VM workflow depends on hosted runners with KVM; a cheap software-Vulkan smoke test is a candidate
  addition (`devel/vulkan/TODO.md`).

## Open items from finished topics

The topics below are finished and their directories were removed (`devel/CHANGELOG.md` names the commit that
still has each). Their open items are kept here, one row each, tagged `(from: <topic>)`; an item shared by two topics
is one row with both tags.

### AArch64 and the VM PAL

| Item | From |
|---|---|
| `_PalThreadResume()` in the shared VM PAL (`pal/src/host/vm/pal_threading.c`) is a stub: a thread blocked in a syscall is never woken by an asynchronous signal; `test_060_sigaltstack`, `test_150_itimer`, `test_404_sigterm_multithread` hang (skips `[VM:no-thread-resume]`). Done on AArch64 on `dev-aarch64-multiproc-fp` (merge pending); still open for the x86_64 VM PAL | (from: aarch64/vm) |
| Finer timer: CPU0's timer is armed for the earliest timeout (done 2026-10-17, `devel/vm-timer/`); `rwlock` still does not finish (its VM fault is not a timer matter, skip `[VM:slow-timer]` stays); open: 100 ms pipe round trips at 4 vCPUs | (from: aarch64/vm) |
| `test_023_readdir` xfail: `getdents`/`open` on a virtio-fs directory returns `EBADF` (0 of 20 standalone, passed in one full-suite run); `test_021_getdents_large_dir` timed out once. Start in `pal/src/host/vm-common/pal_common_files.c` | (from: aarch64/vm) |
| `test_030_fdleak` xfail: `open(fname, O_RDONLY)` returns `EACCES` on the shared filesystem (mode bits or uid under virtiofsd `--sandbox none`?), plus a spurious QEMU `virtio_bus_start_ioeventfd: failed` line | (from: aarch64/vm) |
| `test_092_sighandler_sigpipe` xfail: the test expects a nonzero exit (SIGPIPE) but the guest exits 0; check `EPIPE`/SIGPIPE in the single-process PAL and how the launcher recovers the exit code from `[ VM exited with code N ]` | (from: aarch64/vm) |
| Latent bug masked by the `TRACE` breadcrumb in `kernel_context.S`: removing `TRACE 1` at `el1_irq_entry` makes `sched_set_get_affinity` panic (`Reference count dropped below 0`) and fails 5 suite tests; find the cause with gdb on the gdbstub, then drop `TRACE` | (from: aarch64/vm) |
| `dev:` device streams, `/sys`, `loader.log_file` and `/dev/attestation` are not provided by the VM PAL (skips `no-dev-streams`, `no-sysfs`, `no-log-file`, `no-attestation`); decide which are worth implementing | (from: aarch64/vm) |
| `large_mmap` needs a larger page-table pool or 2 MB block mappings (`PT_POOL_TABLES` 2048 tables, about 4 GB) and a guest with more than 6 GB RAM | (from: aarch64/vm) |
| The Python CI example needs a PIE `python3` (Ubuntu's is `ET_EXEC` at 0x400000); the example runner skips it. The same limit blocks python's HTTP check through the relay on AArch64 | (from: aarch64/vm, vm-network) |
| Launcher environment: `gramine-vm` is a bash script and cannot forward the caller's environment faithfully (`PWD`, `_`, `SHLVL`); `test_103_env_from_host` is skipped; the guest hostname is always `localhost` | (from: aarch64/vm) |
| The x86_64 VM PAL does not forward CPU exceptions: a `#DE` panics the guest instead of reaching the LibOS as `SIGFPE` (`test_093_sighandler_divbyzero` xfailed on x86_64); compare `pal_common_exception.c` and `kernel_interrupts.c` for the vectors forwarded | (from: aarch64/vm) |
| x86_64 `test_010_gramine_run_test` hits `Internal memory fault with VMA at 0x17ce00` under the VM PAL (xfailed on x86_64, passes on AArch64); not investigated | (from: aarch64/vm) |
| The per-test x86_64 column of the AArch64 VM comparison (`TBD (x86)` cells) was never filled; only the suite totals exist | (from: aarch64/vm) |
| The repo's QEMU fork (10.1.50) does not build for aarch64 (x86 asm in `libos/src/graphics/xcb.c`, `KVM_VCPU_TSC_CTRL` in `util/sg-listener.c`); the AArch64 numbers use stock QEMU 8.2.2. Build a stock QEMU 10.x in the container and rerun the suite with `QEMU_PATH`, or port the fork's two x86-only files | (from: aarch64/vm) |

### VM networking (vsock relay)

| Item | From |
|---|---|
| Relay throughput is about 12 % (GX10) to 36 % (x86_64 workstation) of native redis: find out why (Python relay, one guest vCPU, vsock path) and whether a faster data path is worth it; many small connections and concurrent streams are unmeasured | (from: vm-network) |
| Untested: IPv6 destinations, `SO_REUSEPORT`, UDP policy denial, other resolver configurations | (from: vm-network) |
| Reaching guest servers from the real host: container ports must be published when the container is created and a way to do that is missing; all results use clients in the same rootless container | (from: vm-network) |
| `SO_SNDTIMEO` is stored but not enforced by the VM PAL's blocking send | (from: vm-network) |
| `SIOCGIFCONF` and similar ioctls stay unavailable (the guest has no interfaces); `socket_ioctl` stays skipped | (from: vm-network) |
| AArch64: the error path of `gramine-vm-aarch64` for a missing or inaccessible `/dev/vhost-vsock` with `vm.net` keys was written but not run | (from: vm-network) |
| `gramine-tdx` is built from the same launcher template and installs the relay library into the TDX PAL directory; not built or run | (from: vm-network) |
| Graphics with the relay launcher: `glxgears` and Quake II checked; SuperTuxKart, Minetest and TF2 not, and no frame-rate comparison with the old launcher | (from: vm-network) |
| Two `gramine-vm` launches with the same `GRAMINE_VM_ID_BASE` can pick the same guest CID and race in the virtiofsd pidfile (`/tmp/gramine_vhostfs_<cid>.pid`); the CID needs an atomic reservation | (from: vm-network) |
| A launcher killed with `kill -9` leaves a `/tmp/gramine-vm-relay.*` log file, a stale lock directory and a zombie QEMU; harmless but they accumulate | (from: vm-network) |
| CI cannot build or run the AArch64 VM PAL (no KVM on hosted runners): rerun the AArch64 checks by hand after any change to `pal/src/host/vm-common/` or the launchers; the `loopback` suite is not in the CI workflow | (from: vm-network) |
| The manifest's `vm` table is shared with the VM memory plan on the branch `dev-vm-memory`: `vm.net` is in the schema, so that plan has to extend it | (from: vm-network) |

### RISC-V rewriting and hardware

| Item | From |
|---|---|
| Rewriting on RISC-V hardware, the long run: the default is on since 2026-10-13 after a short run on one board (3.3 to 4.1 times, 66 of 66 unloaded stress runs); not done: escalating stress (40 up to 1,000 runs, 200 or more `threads` runs) under 8-hart load and with different affinities (the emulated bug showed about 1 in 4 `threads` runs under load), an application-level number (redis or lighttpd native, trap, rewritten), other SoCs. The default stays on unless a larger run fails | (from: riscv-hardware, syscall-interception) |
| Hosted RISC-V `test_603_rewrite_stress`: the `threads` scenario hit its 240 s alarm on two hosted runs (`libc=none`, rewriting off) and never in about 90 local runs; hosted workflow deselects it. Dump every thread's stack and wait state from a run over 60 s, vary one factor per run, then re-enable the test in `.github/workflows/direct-riscv64-emulated.yml`; also run at least 200 `threads` runs on hardware. Do not weaken the test | (from: riscv-hardware, syscall-interception) |
| `gettimeofday` accounting on RISC-V: with rewriting on the test's wall time does not change and strace shows about 0.72 `rt_sigreturn` per call with no `gettimeofday` kernel arrival; identify the trapping instruction (`strace -e trace=signal`, the `syscall-patch` debug log), run on hardware with rewriting on and off, and rule out an exemption the rewriter cannot reach (vDSO, internal fast path) before claiming a gap in the patcher | (from: riscv-hardware, syscall-interception) |
| Rust example on RISC-V is unverified (no `cargo` in the emulated guest or on the board) | (from: riscv-hardware, syscall-interception) |
| `riscv_flush_icache` and the seccomp filter: what the deployed filter does with that call from the PAL, and which PAL primitive the LibOS should call to flush other harts' instruction caches (today `pal_arch_flush_icache`), is unexamined | (from: syscall-interception) |
| Shared-code split deferred: the eligibility rule, the enable flag and the late-handler check exist twice (`libos/src/arch/aarch64/libos_syscall_patch.c`, `libos/src/arch/riscv64/libos_syscall_patch.c`); guard for the split is one `direct-aarch64.yml` run (LibOS suite and the `stress` input) | (from: syscall-interception) |
| AArch64 stub-page hardening: whether the pages should become read-execute after being written (the RISC-V allocator uses one immutable page per stub; the AArch64 one reuses read-write-execute pages) | (from: syscall-interception) |
| Reach of other libraries against the +-1 MiB `jal` reach (Python extension modules, `libstdc++`); only the bundled glibc and `ld.so` were counted | (from: syscall-interception) |
| `libc=none` builds and `rewrite_stress`: a VM-enabled x86_64 build runs the test against the host's libc and its blob and jit scenarios stay skipped by architecture; correct but not exercised in CI | (from: syscall-interception) |
| x86_64 evaluation of rewriting (phase 6) stays optional; revisit if static or Go binaries often hit the trap path | (from: syscall-interception) |
| Board record missing: `hwprobe` values, cache-block sizes from the device tree, a sustained-load thermal and frequency record | (from: riscv-hardware) |
| The other example suites on hardware (redis, memcached, lighttpd, nginx, networks, rust) were not run; only helloworld, multithread, busybox, bash, python and sqlite | (from: riscv-hardware) |
| Read the 40 compiler warning lines of the native build log on the board | (from: riscv-hardware) |
| The first LibOS run on the board counted 181 tests, the clean run 180; unexplained | (from: riscv-hardware) |
| `test_054_large_mmap` cannot pass on a 3.7 GiB board under the default overcommit rule: decide whether a skip condition for small-memory hosts belongs in the test (a shared-code change, not made) | (from: riscv-hardware) |
| Smaller RISC-V items (phase 8): `test_102_cpuinfo`, a `statx` regression test, the debugger library listing, as far as the board allows | (from: riscv-hardware) |

## Topic lists

| Topic | Open items |
|---|---|
| AArch64 direct PAL | `devel/aarch64/direct/TODO.md` |
| `AT_HWCAP`/`AT_HWCAP2` on AArch64 | `devel/aarch64/hwcap/TODO.md` |
| RISC-V direct PAL | `devel/riscv/TODO.md` |
| Graphics host redirection (vm) | `devel/graphics/TODO.md` |
| Accelerated graphics for `gramine-direct` | `devel/graphics-direct/TODO.md` |
| Graphics on AArch64 | `devel/graphics-aarch64/TODO.md` |
| Vulkan | `devel/vulkan/TODO.md` |
| Multi-process support in the VM PAL | `devel/vm-multiproc/TODO.md` |
| Missing LibOS system calls | `devel/libos-syscalls/TODO.md` |
| Syscall compatibility of `gramine-vm` | `devel/syscall-compat/TODO.md` |
| Application profiling | `devel/syscall-profile/TODO.md` |
| Host libc instead of a built glibc | `devel/runtime-libc/TODO.md` |
| Windows VBS enclaves | `devel/vbs-enclave/TODO.md` |
| Quickstart guides | `devel/quickstart/TODO.md` |
| Documentation refactor | `devel/docs-refactor/TODO.md` |
