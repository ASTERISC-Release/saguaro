# Quickstart guides

One short guide per platform, each from a fresh checkout to a running example, with the commands
that were run on a machine of that platform (each guide ends with what was verified, where and
when). The complete instructions are in [INSTALL.md](../../INSTALL.md); the platform table
and status of this tree are in [README.md](../../README.md).

Symbols as in `README.md`: ✅ supported, ⚠️ experimental, 🛫 planned, ❌ not supported, ➖ not
applicable.

| What | Intel x86_64 | AMD64 | AArch64 | RISC-V (RV64) | Guide |
|----|----|----|----|----|----|
| `gramine-direct`: an ordinary Linux process; hello world, regression tests | ✅ | ✅ | ✅ | ⚠️ [riscv.md](riscv.md) | [direct.md](direct.md) |
| `gramine-vm`: a microVM under KVM; processes, VM size, networking | ✅ | ⚠️ | ⚠️ | 🛫 | [vm.md](vm.md) |
| `gramine-direct` with the host GPU: glxgears, Quake II, Vulkan | ⚠️ | ⚠️ | ⚠️ (NVIDIA) | ❌ | [graphics-direct.md](graphics-direct.md) |
| `gramine-vm` with graphics redirection: glxgears, Quake II (OpenGL, Vulkan) | ⚠️ | ⚠️ | 🛫 | ❌ | [graphics-vm.md](graphics-vm.md) |
| Windows VBS enclaves (`gramine-vbs`): hello world, Python, attestation | ❌ | ❌ | ⚠️ (Windows 11) | ➖ | [vbs-windows.md](vbs-windows.md) |
| AMD SEV-SNP (`gramine-sev`): hello world, regression tests, attestation | ➖ | ⚠️ | ➖ | ➖ | [sev.md](sev.md) |
| Intel TDX and SGX (inherited; not verified here) | ✅ | ➖ | ➖ | ➖ | [tdx-sgx.md](tdx-sgx.md) |

## Which one do I need?

- **Just trying Gramine, or developing it**: [direct.md](direct.md). It needs only
  `podman` (or the build packages) and works anywhere, also in a VM or a container.
- **The application should not run on the host's kernel**: [vm.md](vm.md). It needs a
  physical Linux machine of the same architecture with KVM.
- **A game or another OpenGL/Vulkan program**: on your own desktop and with trusted code,
  [graphics-direct.md](graphics-direct.md) (fastest, but not isolated; also AArch64 with
  NVIDIA); inside a microVM on x86_64, [graphics-vm.md](graphics-vm.md) (needs the QEMU
  fork).
- **A RISC-V build**, without RISC-V hardware: [riscv.md](riscv.md).
- **An application protected from Windows itself**, on an ARM64 Windows 11 PC:
  [vbs-windows.md](vbs-windows.md).
- **Intel confidential computing**: [tdx-sgx.md](tdx-sgx.md), pointers to the inherited
  documents.
- **AMD confidential computing** (SEV-SNP, with attestation): [sev.md](sev.md). It needs an
  EPYC host with SEV-SNP and access to `/dev/sev`.
