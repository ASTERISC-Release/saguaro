# Documentation

This directory has two kinds of documents: those written for Saguaro, and reference pages
inherited from upstream Gramine. The upstream Sphinx site is no longer built here; the inherited
pages were converted to Markdown, and a few traces of Sphinx markup remain in them.
Upstream's own documentation is at <https://gramine.readthedocs.io>.

Start with `../README.md` (what this tree is, platform matrix), `quickstart/README.md` (one short
guide per platform), `FEATURES.md` (what it adds) and `../INSTALL.md` (build, install, containers,
the RISC-V cross-build). `quickstart/vbs-enable.md` explains how to enable VBS on a Windows machine.

## Written for this tree

| Document | Content |
|---|---|
| `FEATURES.md` | what this tree adds to gramine-tdx, in one or two lines each |
| `quickstart/README.md` | index of the quickstart guides: platform and architecture to guide, "which one do I need" |
| `TODO.md` | open items, by architecture |
| `AGENTS.md` | how to develop in this tree (parallel checkouts, plans, testing, reporting); how to run each mode is in `../AGENTS.md` |
| `manifest-keys.md` | index of documented manifest keys, their applicability and purpose |
| `quickstart/direct.md`, `quickstart/vm.md` | `saguaro-direct` and `saguaro-vm` on x86_64 and AArch64: install, hello world, tests |
| `quickstart/sev.md` | `saguaro-sev` on AMD SEV and SEV-SNP: install, hello world, attestation, tests |
| `GRAPHICS.md` | graphics in brief: the direct and the redirected route, the pointer-table issue and its fix, the guard, the Proton game results |
| `graphics/README.md` | summaries of the graphics development documents, one page per topic: `graphics/vm.md`, `graphics/direct.md`, `graphics/aarch64.md`, `graphics/vulkan.md`, `graphics/proton.md` |
| `quickstart/graphics-direct.md`, `quickstart/graphics-vm.md` | graphics under `saguaro-direct` (x86_64, AArch64 with NVIDIA) and under `saguaro-vm` (x86_64) |
| `quickstart/riscv.md`, `quickstart/vbs-windows.md`, `quickstart/vbs-enable.md`, `quickstart/tdx-sgx.md` | the RISC-V cross-build and test guest; Windows VBS enclaves and enabling VBS on a machine; the SGX and TDX back ends |

## Inherited from upstream

| Document | Content | Note |
|---|---|---|
| `manifest-syntax.md` | detailed syntax, defaults, constraints and examples for manifest keys | inherited, with tree-specific additions; indexed by `manifest-keys.md` |
| `glossary.md` | terms | |
| `attestation.md` | Intel SGX attestation | inherited; the SGX route of this tree: `quickstart/tdx-sgx.md` |
