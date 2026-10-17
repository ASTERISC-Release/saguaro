# TODO

Open items are kept per topic, in the `TODO.md` of each directory under `Documentation/devel/`
(the table at the end lists them). This page is the **map by architecture and host**: for each
combination it names the main open items and points to the topic list with the details. It is kept
short on purpose; an item that is fixed is removed from the topic list first and then from here.
`FEATURES.md` lists the planned features in one line each.

## x86_64

| Host / PAL | Main open items | Details |
|---|---|---|
| `direct` | audit PAL state that a starting child allocates and frees at exit (the shared file cache was one case); sound and the remaining GPU-driver gaps for graphics | `devel/vm-multiproc/TODO.md`, `devel/graphics-direct/TODO.md` |
| `vm` | robust mutex for the System V shared tables; `/proc/<pid>/stat` of other processes; `PROT_NONE` enforcement; finer timer than the 100 ms tick; async signal resume; UDP/IPv6 relay gaps | `devel/vm-multiproc/TODO.md`, `devel/libos-syscalls/TODO.md`, `devel/syscall-compat/TODO.md`, `devel/vm-network/TODO.md` |
| `vm` with graphics (OpenGL, Vulkan) | present events and a non-flipping blit for Vulkan, idle CPU of the listener, two-thread submission cost, SuperTuxKart start hang, sound, Team Fortress 2 | `devel/graphics/TODO.md`, `devel/vulkan/TODO.md` |
| `tdx` and `sgx` | never run on this tree's hardware; extend the interrupt-before-IDT guard to TDX; the SGX build needs `protobuf-c` (not in `dependencies.sh` or the container); the TDX install needs `td-shim` | `devel/vm-multiproc/TODO.md`, `quickstart/tdx-sgx.rst` |
| Proton (Wine, DXVK) | phase 1 steps 1 to 3 done 2026-10-15 on `dev-proton-vm-p1` (Unix-socket nodes, fork of reservations, `SCM_RIGHTS`); next: the `%gs` decision (`devel/proton-vm/design-gs.md`), then interruptible blocking calls and exception vectors in the VM PAL | `devel/proton-vm/PLAN.md` |

## AArch64

| Host / PAL | Main open items | Details |
|---|---|---|
| `direct` | `kill11` hang; longer soak runs and real applications (openssh, dropbear); ABI-conformance tests | `devel/syscall-compat/TODO.md`, `devel/aarch64/direct/TODO.md` |
| `vm` | multi-process (phases 0 to 3 merged; phases 4 and 5 on `dev-aarch64-multiproc-fp`: interruptible PAL waits, EL1 LibOS faults end the process instead of the guest, 4-vCPU forks as fast as 2; the AArch64 FP/SIMD-at-syscall-return and full-sigaltstack bugs fixed 2026-10-16; phase 5 qualification done 2026-10-16 (`devel/vm-multiproc-aarch64/MATRIX-MULTIPROCESS.md`), merge pending; open: the pool's identity range; `devel/vm-multiproc-aarch64/`); console and socket waits not interruptible yet; timer granularity; `dev:`/`/sys`/attestation streams; 2 MiB pages | `devel/aarch64/vm/TODO.md`, `devel/syscall-compat/TODO.md`, `devel/vm-multiproc-aarch64/TODO.md` |
| `direct` with graphics (NVIDIA proprietary driver) | remaining NVIDIA fd-bearing ioctls; several GPU nodes; Vulkan and CUDA untested; `SCM_RIGHTS` on plain socketpairs; syscall cost of Minetest | `devel/graphics-aarch64/TODO.md` |
| `vm` with graphics | not available: the QEMU fork's listener and the comm-page address are x86-only; redesign deferred (`virtio-gpu` as one option) | `devel/graphics-aarch64/PLAN.md` |
| all AArch64 PALs: CPU feature words | `AT_HWCAP`/`AT_HWCAP2` passed (allowlist); SVE, PAC, BTI, MTE stay masked until the LibOS supports them; EL0 ID-register reads not emulated under vm; VBS features without a `PF_ARM_*` number not advertised | `devel/aarch64/hwcap/TODO.md` |

## RISC-V (RV64)

| Host / PAL | Main open items | Details |
|---|---|---|
| `direct` | validation on hardware (stress under 8-hart load, other example suites); syscall-rewriting stress and the hosted `test_603` timeout; CPU identification in the PAL | `devel/riscv/TODO.md`, `devel/riscv-hardware/TODO.md`, `devel/syscall-interception/TODO.md` |
| `vm` | not available (planned) | `README.rst` |

## Windows 11 on ARM64: VBS enclaves (`vbs` PAL)

| Item | Details |
|---|---|
| **Evaluate the three VBS PAL changes in more detail** (counter-based clock, `CTR_EL0` emulation, skipping the clear of never-used pool memory); the maintainer's first look found them fine | `devel/vbs-enclave/TODO.md` ("Review and x86_64") |
| **Port VBS to x86_64 Windows 11**: done through phase 3 on an x64 machine (2026-10-15: gate, tests, tools, six application examples); regression subset 72/103 (same failures as ARM64); left: x86_64 variants of the static `CI-Examples/vbs` manifests, the patched-glibc fast path, a review | `devel/vbs-enclave/PLAN-x64.md`, `devel/vbs-enclave/TODO.md` ("Review and x86_64") |
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

## Topic lists

| Topic | Open items |
|---|---|
| AArch64 direct PAL | `devel/aarch64/direct/TODO.md` |
| AArch64 vm PAL (and what it shares with the x86_64 vm PAL: thread resume, timer granularity) | `devel/aarch64/vm/TODO.md` |
| `AT_HWCAP`/`AT_HWCAP2` on AArch64 | `devel/aarch64/hwcap/TODO.md` |
| RISC-V direct PAL | `devel/riscv/TODO.md` |
| RISC-V hardware validation | `devel/riscv-hardware/TODO.md` |
| Graphics host redirection (vm) | `devel/graphics/TODO.md` |
| Accelerated graphics for `gramine-direct` | `devel/graphics-direct/TODO.md` |
| Graphics on AArch64 | `devel/graphics-aarch64/TODO.md` |
| Vulkan | `devel/vulkan/TODO.md` |
| Multi-process support in the VM PAL | `devel/vm-multiproc/TODO.md` |
| Missing LibOS system calls | `devel/libos-syscalls/TODO.md` |
| Syscall compatibility of `gramine-vm` | `devel/syscall-compat/TODO.md` |
| Syscall interception | `devel/syscall-interception/TODO.md` |
| Application profiling | `devel/syscall-profile/TODO.md` |
| TCP and UDP in the VM PAL | `devel/vm-network/TODO.md` |
| Host libc instead of a built glibc | `devel/runtime-libc/TODO.md` |
| Windows VBS enclaves | `devel/vbs-enclave/TODO.md` |
| Quickstart guides | `devel/quickstart/TODO.md` |
| Documentation refactor | `devel/docs-refactor/TODO.md` |
