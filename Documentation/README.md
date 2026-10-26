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
| `manifest-keys.md` | index of documented manifest keys, their applicability and purpose |
| `quickstart/direct.md`, `quickstart/vm.md` | `saguaro-direct` and `saguaro-vm` on x86_64 and AArch64: install, hello world, tests |
| `quickstart/sev.md` | `saguaro-sev` on AMD SEV and SEV-SNP: install, hello world, attestation, tests |
| `quickstart/graphics-direct.md`, `quickstart/graphics-vm.md` | graphics under `saguaro-direct` (x86_64, AArch64 with NVIDIA) and under `saguaro-vm` (x86_64) |
| `quickstart/riscv.md`, `quickstart/vbs-windows.md`, `quickstart/vbs-enable.md`, `quickstart/sgx.md`, `quickstart/tdx.md` | the RISC-V cross-build and test guest; Windows VBS enclaves and enabling VBS on a machine; the SGX back end; the TDX back end |

## Inherited from upstream

| Document | Content | Note |
|---|---|---|
