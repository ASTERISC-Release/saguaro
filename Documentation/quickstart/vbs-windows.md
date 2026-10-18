# Quickstart: Windows VBS enclaves (`gramine-vbs`)

The `vbs` PAL runs an unmodified Linux AArch64 application inside a Windows
*virtualization-based security* (VBS) enclave. Because a VBS enclave cannot make memory executable
at run time, the library OS, a glibc, the application and its files are packed into a signed
enclave image when it is built.

## Status

Symbols as in [README.md](../../README.md):

| Configuration | Windows 11 (ARM64) | Windows 11 (x86_64/AMD64) |
|----|----|----|
| `vbs` | ⚠️ experimental | ⚠️ experimental |

## Requirements

- Windows 11 25H2 with **VBS enabled** and development environment setup (including enclave test signing
with a custom certificate and Visual Studio 2022 or later).
  - Refer to [the VBS manual](../vbs.md) for one-time machine setup details.
- **WSL2** with an Ubuntu 24.04 distribution.

# 1. Install

In WSL (Ubuntu 24.04), in a checkout of the repository. No sudo; nothing on Windows is changed:

    scripts/install.sh --vbs

This installs the missing build packages under `~/.local/share/gramine-vbs` (meson, ninja, clang, ...),
puts `gramine-vbs` in `~/.local/bin`, builds the vbs glibc, PAL and LibOS (a few minutes, once), and
checks the Windows side: it ends with `the Windows side is ready`, or lists the one-time Windows steps
that are missing. (The Windows PowerShell form `scripts\vbs\gramine-vbs.cmd setup` also checks both
sides.)

# 2. Example: Hello World

    cd CI-Examples/helloworld
    make VBS=1
    gramine-vbs helloworld

`make VBS=1` builds the program for the enclave and packs it, then links and signs the enclave image
(the signed `helloworld.dll` in the folder `helloworld.vbs`, next to `helloworld.manifest.vbs`). `gramine-vbs helloworld` runs it,
like `gramine-direct helloworld`; if the package is missing or older than its manifest, it says to run
`make VBS=1` (it never builds):

    Hello, world from a VBS enclave (pid 1, 0 arguments)

`make VBS=1 check` does both and compares the output.

# 3. Example: Python

    cd CI-Examples/python
    make VBS=1
    gramine-vbs python

The first `make VBS=1` builds CPython 3.12.7 for the enclave in WSL (about 70 s, once) and packs the
interpreter with its standard library; `hello.py` prints the Python version, a SHA-256 and the
results of three threads.

Other examples with a `vbs.manifest.template`: `CI-Examples/redis`, `memcached`, `lighttpd`, `nginx`
and more (listed in [vbs.md](../vbs.md)); the same two commands build and run them.

# 4. Attestation

`/dev/attestation` inside the enclave returns a VBS attestation report for 64 bytes of user
data, and `gramine-vbs verify` checks one on the same machine:

    cd CI-Examples/vbs/attestation
    make VBS=1
    gramine-vbs attest <64 hex digits>
    gramine-vbs verify attest data/out/report.bin --user-data <64 hex digits> --same-image --min-svn 1

The check is local to this machine; what it establishes and what it does not is in
[vbs.md](../vbs.md), Attestation.

# 5. Troubleshooting

- A setup prerequisite is missing: run `gramine-vbs check` (or `scripts/install.sh --vbs` again) and fix the
  first failed check.
- `gramine-vbs` reports a missing or older package: run `make VBS=1` in the example's folder.
- Your application fails after launch because of process creation or JIT assumptions:
  VBS supports one process per enclave (no `fork`/`vfork`/new-process `execve`) and no run-time code generation.
- For your own app, write a `vbs.manifest.template` with `vbs.packed_files` (what goes into the image)
  and a Makefile that includes `scripts/vbs/Makefile.vbs`; start from `CI-Examples/helloworld` and
  [vbs.md](../vbs.md).
