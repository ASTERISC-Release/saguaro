# Documentation

This directory has two kinds of documents: those written for this tree, and reference pages
inherited from upstream Gramine. The upstream Sphinx site is no longer built here; the inherited
pages remain as plain reStructuredText, and a few Sphinx-only directives in them do not render.
Upstream's own documentation is at <https://gramine.readthedocs.io>.

Start with `../README.md` (what this tree is, platform matrix), `quickstart/README.md` (one short
guide per platform), `FEATURES.md` (what it adds) and `../INSTALL.md` (build, install, containers,
the RISC-V cross-build). `vbs.md` is the manual for Windows VBS enclaves.

## Written for this tree

| Document | Content |
|---|---|
| `FEATURES.md` | what this tree adds to gramine-tdx, and what is planned, in one or two lines each |
| `quickstart/README.md` | index of the quickstart guides: platform and architecture to guide, "which one do I need" |
| `will-it-run.md` | checking whether an application is likely to run: the syscall profiler and the tolerance check |
| `manifest-keys.md` | index of documented manifest keys, their applicability and purpose |
| `quickstart/direct.md`, `quickstart/vm.md` | `gramine-direct` and `gramine-vm` on x86_64 and AArch64: install, hello world, tests |
| `quickstart/graphics-direct.md`, `quickstart/graphics-vm.md` | graphics under `gramine-direct` (x86_64, AArch64 with NVIDIA) and under `gramine-vm` (x86_64) |
| `quickstart/riscv.md`, `quickstart/vbs-windows.md`, `quickstart/tdx-sgx.md` | the RISC-V cross-build and test guest; Windows VBS enclaves; pointers for the inherited TDX and SGX back ends |
| `vbs.md` | `gramine-vbs` manual: Windows installation (ARM64 and x64), quick start, the tool, attestation, limits |

## Inherited from upstream

| Document | Content | Note |
|---|---|---|
| `manifest-syntax.md` | detailed syntax, defaults, constraints and examples for manifest keys | inherited, with tree-specific additions; indexed by `manifest-keys.md` |
| `pal/host-abi.md` | the PAL interface | the function list needs Doxygen; see `pal/include/pal/pal.h` |
| `manpages/` | the command-line tools | not built into man pages here |
| `python/api.md` | the `graminelibos` Python API | the class list needs Sphinx; see `python/graminelibos/` |
| `glossary.md` | terms | |
| `attestation.md`, `sgx-intro.md`, `sgx-setup.md`, `performance.md`, `python/writing-sgx-sign-plugins.md` | Intel SGX | the SGX backend is built but not tested here |
