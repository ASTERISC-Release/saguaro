# Documentation

This directory has two kinds of documents: those written for this tree, and reference pages
inherited from upstream Gramine. The upstream Sphinx site is no longer built here; the inherited
pages remain as plain reStructuredText, and a few Sphinx-only directives in them do not render.
Upstream's own documentation is at <https://gramine.readthedocs.io>.

Start with `../README.rst` (what this tree is, platform matrix) and `../INSTALL.rst` (build,
install, containers, the RISC-V cross-build).

## Written for this tree

| Document | Content |
|---|---|
| `FEATURES.md` | what this tree adds to gramine-tdx, and what is planned, in one or two lines each |
| `devel/multiarch.md` | platform matrix in detail, build options, how to test, CI workflows |
| `devel/vm.md` | building and running the vm PAL (`gramine-vm`) |
| `devel/graphics/tirith-delta.md` | changes against upstream gramine-tdx |
| `devel/summary.md` | outcome of each port in one page, with pointers into the logs |
| `devel/README.md` | the layout of the development documents: `PLAN.md`, `FINDINGS.md`, `TODO.md`, `TESTING.md` per topic |
| `devel/aarch64/direct/`, `devel/aarch64/vm/`, `devel/riscv/`, `devel/graphics/` | plan, findings, open items and test results of each port |
| `TODO.md` | index of the per-topic open items |

## Inherited from upstream

| Document | Content | Note |
|---|---|---|
| `manifest-syntax.rst` | every manifest option | extended here (`sys.rewrite_syscalls`) |
| `pal/host-abi.rst` | the PAL interface | the function list needs Doxygen; see `pal/include/pal/pal.h` |
| `manpages/` | the command-line tools | not built into man pages here |
| `python/api.rst` | the `graminelibos` Python API | the class list needs Sphinx; see `python/graminelibos/` |
| `devel/coding-style.rst`, `devel/new-syscall.rst`, `devel/debugging.rst` | conventions, adding a syscall, debugging with GDB | |
| `glossary.rst` | terms | |
| `attestation.rst`, `sgx-intro.rst`, `sgx-setup.rst`, `performance.rst`, `python/writing-sgx-sign-plugins.rst` | Intel SGX | the SGX backend is built but not tested here |
