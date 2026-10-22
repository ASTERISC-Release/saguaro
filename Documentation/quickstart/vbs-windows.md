# Quickstart: Windows VBS enclaves (`saguaro-vbs`)

The `vbs` PAL runs an unmodified Linux application (AArch64 or x86_64, as the Windows machine) inside a Windows
*virtualization-based security* (VBS) enclave. Because a VBS enclave cannot make memory executable
at run time, the library OS, a glibc, the application and its files are packed into a signed
enclave image when it is built.

## Requirements

- Windows 11 25H2 (ARM64 or x64) with [**VBS enabled and development environment setup**](vbs-enable.md).
- **WSL2** with a Ubuntu 24.04 or Ubuntu 26.04 distribution.

# 1. Install

In WSL, in a checkout of the repository. No sudo; nothing on Windows is changed:

    cd <repo-home>
    scripts/install.sh --vbs

This installs the missing build packages under `~/.local/share/saguaro-vbs` (meson, ninja, clang, ...),
puts `saguaro-vbs` in `~/.local/bin`, builds the vbs glibc, PAL and LibOS (a few minutes, once), and
checks the Windows side: it ends with `the Windows side is ready`, or lists the one-time Windows steps
that are missing. 

# 2. Example: Hello World

In WSL, in the directory of the example:

    cd CI-Examples/helloworld
    ./exec.sh --vbs

`./exec.sh --vbs` runs `make VBS=1` and `make VBS=1 check` (below). It stops with a message when the
runtime is not installed or the host is not a WSL on a Windows machine with VBS enclaves.
The steps by hand:

    cd CI-Examples/helloworld
    make VBS=1
    saguaro-vbs helloworld

`make VBS=1` builds the program for the enclave and packs it, then links and signs the enclave image
(the signed `helloworld.dll` in the folder `helloworld.vbs`, next to `helloworld.manifest.vbs`). 

    Hello, world from a VBS enclave (pid 1, 0 arguments)

# 3. Example: Python

    cd CI-Examples/python
    ./exec.sh --vbs

By hand:

    make VBS=1
    saguaro-vbs python

The first `make VBS=1` builds CPython 3.12.7 for the enclave in WSL (once) and packs the
interpreter with its standard library; `hello.py` prints the Python version, a SHA-256 and the
results of three threads.

Other examples with a `vbs.manifest.template` include `CI-Examples/redis`, `memcached`, `lighttpd`, `nginx`
and more.

# 4. Attestation

`/dev/attestation` inside the enclave returns a VBS attestation report for 64 bytes of user
data, and `saguaro-vbs verify` checks one on the same machine:

    cd CI-Examples/vbs/attestation
    make VBS=1
    saguaro-vbs attest <64 hex digits>
    saguaro-vbs verify attest data/out/report.bin --user-data <64 hex digits> --same-image --min-svn 1

The check is local to this machine; what it establishes and what it does not is in
`dev docs: vbs-enclave/manual.md`, Attestation.

# 5. Troubleshooting

- A setup prerequisite is missing: run `saguaro-vbs check` (or `scripts/install.sh --vbs` again) and fix the
  first failed check.
- `make VBS=1` stops with an install message: the runtime is missing; run `scripts/install.sh --vbs`.
- `./exec.sh --vbs` says the host cannot run `vbs`: it needs WSL on a Windows machine with VBS
  enclaves and the installed runtime (`build-vbs/runtime.stamp`).
- `saguaro-vbs` reports a missing or older package: run `make VBS=1` in the example's folder.
- The application fails after launch because of process creation or JIT assumptions:
  VBS supports one process per enclave (no `fork`/`vfork`/new-process `execve`) and no run-time code generation.
- For an application of the user, write a `vbs.manifest.template` with `vbs.packed_files` (what goes into the image)
  and a Makefile that includes `scripts/vbs/Makefile.vbs`; start from `CI-Examples/helloworld` and
  `dev docs: vbs-enclave/manual.md`.
