# Quickstart guides

One guide per platform, each from a checkout to a running example. The complete instructions are in [INSTALL.md](../../INSTALL.md); the platform table
and status of this tree are in [README.md](../../README.md).

Symbols as in `README.md`: ✅ supported, ⚠️ experimental, 🛫 planned, ❌ not supported, ➖ not
applicable.

| What | Intel x86_64 | AMD64 | AArch64 | RISC-V (RV64) | Guide |
|----|----|----|----|----|----|
| `gramine-direct`: an ordinary Linux process; hello world, regression tests | ✅ | ✅ | ✅ | ⚠️ [riscv.md](riscv.md) | [direct.md](direct.md) |
| `gramine-vm`: a microVM under KVM; processes, VM size, networking | ✅ | ⚠️ | ⚠️ | 🛫 | [vm.md](vm.md) |
| `gramine-direct` with the host GPU: glxgears, Quake II, Vulkan | ⚠️ | ⚠️ | ⚠️ (NVIDIA) | ❌ | [graphics-direct.md](graphics-direct.md) |
| `gramine-vm` with graphics redirection: glxgears, Quake II (OpenGL, Vulkan) | ⚠️ | ⚠️ | 🛫 | ❌ | [graphics-vm.md](graphics-vm.md) |
| Windows VBS enclaves (`gramine-vbs`): hello world, Python, attestation | ⚠️ (Windows 11 x64) | ⚠️ (Windows 11 x64) | ⚠️ (Windows 11 ARM64) | ➖ | [vbs-windows.md](vbs-windows.md); machine setup: [vbs-enable.md](vbs-enable.md) |
| AMD SEV-SNP (`gramine-sev`): hello world, regression tests, attestation | ➖ | ⚠️ | ➖ | ➖ | [sev.md](sev.md) |
| Intel SGX and TDX | ⚠️ | ➖ | ➖ | ➖ | [tdx-sgx.md](tdx-sgx.md) |

## The route of all guides

- **The container is the only route.** `container/run.sh` opens a shell in the development
  container of this machine's architecture, `container/run.sh '<command>'` runs one command in it,
  and `container/run.sh --target riscv64 ...` opens the RISC-V cross container (see
  [container/README.md](../../container/README.md)). Nothing is built or installed on the host.
  The one exception is the Windows VBS enclave ([vbs-windows.md](vbs-windows.md)), which runs in
  WSL on a Windows machine.
- **Installation is `scripts/install.sh`** (there is no top-level `install.sh`). No option installs
  the direct backend into `~/.local/saguaro`; `--vm` the vm PAL into `~/.local/saguaro-vm`;
  `--sev --graphics disabled` the vm PAL with the SEV PAL and `gramine-sev`; `--sgx` the SGX backend into
  `~/.local/saguaro-sgx`; `--vbs` the WSL side of
  the VBS tooling and runtime. `container/run.sh` loads `~/.local/saguaro-vm/env.sh` itself when
  it exists.
- **Examples run through `./exec.sh`** in the directory of the example under `CI-Examples/`:
  `./exec.sh --direct | --vm | --sev | --sev-snp | --sgx | --vbs`. A backend is required. See the
  "Testing" section of [CI-Examples/README.md](../../CI-Examples/README.md) and the recorded results
  in [RESULTS.md](../../CI-Examples/RESULTS.md).

## Which one do I need?

- **Trying Saguaro, or developing it**: [direct.md](direct.md). It needs only
  `podman` and works anywhere, also in a VM or a container.
- **The application should not run on the host's kernel**: [vm.md](vm.md). It needs a
  physical Linux machine of the same architecture with KVM.
- **A game or another OpenGL/Vulkan program**: on a desktop with trusted code,
  [graphics-direct.md](graphics-direct.md) (fastest, but not isolated; also AArch64 with
  NVIDIA); inside a microVM on x86_64, [graphics-vm.md](graphics-vm.md) (needs the QEMU
  fork, kept outside this repository).
- **A RISC-V build**, without RISC-V hardware: [riscv.md](riscv.md).
- **An application protected from Windows itself**, on an ARM64 or x64 Windows 11 PC, in WSL (the one guide without the container):
  [vbs-windows.md](vbs-windows.md).
- **Intel confidential computing**: [tdx-sgx.md](tdx-sgx.md). SGX: the container route,
  `scripts/install.sh --sgx`, `./exec.sh --sgx`. TDX: `scripts/install.sh --vm --tdx` and `gramine-tdx`; the
  examples that run on SEV also run on TDX.
- **AMD confidential computing** (SEV-SNP, with attestation): [sev.md](sev.md). It needs an
  EPYC host with SEV-SNP and access to `/dev/sev`.
