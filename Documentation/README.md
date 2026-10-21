# Documentation

This directory has two kinds of documents: those written for this tree, and reference pages
inherited from upstream Gramine. The upstream Sphinx site is no longer built here; the inherited
pages remain as plain reStructuredText, and a few Sphinx-only directives in them do not render.
Upstream's own documentation is at <https://gramine.readthedocs.io>.

Start with `../README.md` (what this tree is, platform matrix), `quickstart/README.md` (one short
guide per platform), `FEATURES.md` (what it adds) and `../INSTALL.md` (build, install, containers,
the RISC-V cross-build). `quickstart/vbs-enable.md` explains how to enable VBS on a Windows machine.

## Written for this tree

| Document | Content |
|---|---|
| `FEATURES.md` | what this tree adds to gramine-tdx, in one or two lines each |
| `quickstart/README.md` | index of the quickstart guides: platform and architecture to guide, "which one do I need" |
| `will-it-run.md` | checking whether an application is likely to run: the syscall profiler and the tolerance check |
| `manifest-keys.md` | index of documented manifest keys, their applicability and purpose |
| `quickstart/direct.md`, `quickstart/vm.md` | `gramine-direct` and `gramine-vm` on x86_64 and AArch64: install, hello world, tests |
| `GRAPHICS.md` | the graphics architecture (direct and redirected routes, the pointer table of demand-paged processes), the Intel pointer-table issue and its fix, results of Windows games under Proton |
| `quickstart/graphics-direct.md`, `quickstart/graphics-vm.md` | graphics under `gramine-direct` (x86_64, AArch64 with NVIDIA) and under `gramine-vm` (x86_64) |
| `quickstart/riscv.md`, `quickstart/vbs-windows.md`, `quickstart/vbs-enable.md`, `quickstart/tdx-sgx.md` | the RISC-V cross-build and test guest; Windows VBS enclaves and enabling VBS on a machine; pointers for the inherited TDX and SGX back ends |

## Inherited from upstream

| Document | Content | Note |
|---|---|---|
| `manifest-syntax.md` | detailed syntax, defaults, constraints and examples for manifest keys | inherited, with tree-specific additions; indexed by `manifest-keys.md` |
| `manpages/` | the command-line tools | not built into man pages here |
| `glossary.md` | terms | |
| `attestation.md`, `sgx-intro.md`, `sgx-setup.md` | Intel SGX | inherited; the SGX backend is tested on one machine only (see the SGX quickstart) |
