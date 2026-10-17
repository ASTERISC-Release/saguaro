# Documentation

This directory has two kinds of documents: those written for this tree, and reference pages
inherited from upstream Gramine. The upstream Sphinx site is no longer built here; the inherited
pages remain as plain reStructuredText, and a few Sphinx-only directives in them do not render.
Upstream's own documentation is at <https://gramine.readthedocs.io>.

Start with `../README.rst` (what this tree is, platform matrix), `quickstart/README.rst` (one short
guide per platform), `FEATURES.md` (what it adds) and `../INSTALL.rst` (build, install, containers,
the RISC-V cross-build). `vbs.rst` is the manual for Windows VBS enclaves.

## Written for this tree

| Document | Content |
|---|---|
| `FEATURES.md` | what this tree adds to gramine-tdx, and what is planned, in one or two lines each |
| `quickstart/README.rst` | index of the quickstart guides: platform and architecture to guide, "which one do I need" |
| `will-it-run.rst` | checking whether an application is likely to run: the syscall profiler and the tolerance check |
| `manifest-keys.md` | index of documented manifest keys, their applicability and purpose |
| `quickstart/direct.rst`, `quickstart/vm.rst` | `gramine-direct` and `gramine-vm` on x86_64 and AArch64: install, hello world, tests |
| `quickstart/graphics-direct.rst`, `quickstart/graphics-vm.rst` | graphics under `gramine-direct` (x86_64, AArch64 with NVIDIA) and under `gramine-vm` (x86_64) |
| `quickstart/riscv.rst`, `quickstart/vbs-windows.rst`, `quickstart/tdx-sgx.rst` | the RISC-V cross-build and test guest; Windows VBS enclaves; pointers for the inherited TDX and SGX back ends |
| `vbs.rst` | `gramine-vbs` manual: Windows installation (ARM64 and x64), quick start, the tool, attestation, limits |
| `devel/multiarch.md` | platform matrix in detail, build options, how to test, CI workflows |
| `devel/vm.md` | building and running the vm PAL (`gramine-vm`) |
| `devel/graphics/tirith-delta.md` | changes against upstream gramine-tdx |
| `devel/summary.md` | outcome of each port in one page, with pointers into the logs |
| `devel/README.md` | the layout of the development documents: `PLAN.md`, `FINDINGS.md`, `TODO.md`, `TESTING.md` per topic |
| `devel/<topic>/` (`aarch64/direct`, `aarch64/vm`, `aarch64/hwcap`, `riscv`, `riscv-hardware`, `graphics`, `graphics-direct`, `graphics-aarch64`, `vulkan`, `proton-vm`, `libos-syscalls`, `vm-multiproc`, `vm-multiproc-aarch64`, `syscall-interception`, `runtime-libc`, `vm-network`, `syscall-compat`, `syscall-profile`, `vbs-enclave`, `docs-refactor`, `quickstart`) | plan, findings, open items and test results of each topic |
| `TODO.md` | index of the per-topic open items |

## Inherited from upstream

| Document | Content | Note |
|---|---|---|
| `manifest-syntax.rst` | detailed syntax, defaults, constraints and examples for manifest keys | inherited, with tree-specific additions; indexed by `manifest-keys.md` |
| `pal/host-abi.rst` | the PAL interface | the function list needs Doxygen; see `pal/include/pal/pal.h` |
| `manpages/` | the command-line tools | not built into man pages here |
| `python/api.rst` | the `graminelibos` Python API | the class list needs Sphinx; see `python/graminelibos/` |
| `devel/coding-style.rst`, `devel/new-syscall.rst`, `devel/debugging.rst` | conventions, adding a syscall, debugging with GDB | |
| `glossary.rst` | terms | |
| `attestation.rst`, `sgx-intro.rst`, `sgx-setup.rst`, `performance.rst`, `python/writing-sgx-sign-plugins.rst` | Intel SGX | the SGX backend is built but not tested here |
