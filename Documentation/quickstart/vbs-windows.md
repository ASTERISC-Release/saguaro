# Quickstart: Windows VBS enclaves (`gramine-vbs`)

The `vbs` PAL runs an unmodified Linux AArch64 application inside a Windows
*virtualization-based security* (VBS) enclave. Because a VBS enclave cannot make memory executable
at run time, the library OS, a glibc, the application and its files are packed into a signed
enclave image when it is built.

## Requirements

- Windows 11 25H2 (x86 or Aarch64) with [**VBS enabled and development environment setup**](vbs-enable.md).
- **WSL2** with a Ubuntu 24.04 or Ubuntu 26.04 distribution.

# 1. Install

In WSL (Ubuntu 24.04), in a checkout of the repository. No sudo; nothing on Windows is changed:

    cd <repo-home>
    scripts/install.sh --vbs

This installs the missing build packages under `~/.local/share/gramine-vbs` (meson, ninja, clang, ...),
puts `gramine-vbs` in `~/.local/bin`, builds the vbs glibc, PAL and LibOS (a few minutes, once), and
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
    gramine-vbs helloworld

`make VBS=1` builds the program for the enclave and packs it, then links and signs the enclave image
(the signed `helloworld.dll` in the folder `helloworld.vbs`, next to `helloworld.manifest.vbs`). 

    Hello, world from a VBS enclave (pid 1, 0 arguments)

# 3. Example: Python

    cd CI-Examples/python
    ./exec.sh --vbs

By hand:

    make VBS=1
    gramine-vbs python

The first `make VBS=1` builds CPython 3.12.7 for the enclave in WSL (about 70 s, once) and packs the
interpreter with its standard library; `hello.py` prints the Python version, a SHA-256 and the
results of three threads.

Other examples with a `vbs.manifest.template` include `CI-Examples/redis`, `memcached`, `lighttpd`, `nginx`
and more.

# 4. Attestation

`/dev/attestation` inside the enclave returns a VBS attestation report for 64 bytes of user
data, and `gramine-vbs verify` checks one on the same machine:

    cd CI-Examples/vbs/attestation
    make VBS=1
    gramine-vbs attest <64 hex digits>
    gramine-vbs verify attest data/out/report.bin --user-data <64 hex digits> --same-image --min-svn 1

The check is local to this machine; what it establishes and what it does not is in
`dev docs: vbs-enclave/manual.md`, Attestation.

# 5. Troubleshooting

- A setup prerequisite is missing: run `gramine-vbs check` (or `scripts/install.sh --vbs` again) and fix the
  first failed check.
- `make VBS=1` stops with an install message: the runtime is missing; run `scripts/install.sh --vbs`.
- `./exec.sh --vbs` says the host cannot run `vbs`: it needs WSL on a Windows machine with VBS
  enclaves and the installed runtime (`build-vbs/runtime.stamp`).
- `gramine-vbs` reports a missing or older package: run `make VBS=1` in the example's folder.
- The application fails after launch because of process creation or JIT assumptions:
  VBS supports one process per enclave (no `fork`/`vfork`/new-process `execve`) and no run-time code generation.
- For an application of the user, write a `vbs.manifest.template` with `vbs.packed_files` (what goes into the image)
  and a Makefile that includes `scripts/vbs/Makefile.vbs`; start from `CI-Examples/helloworld` and
  `dev docs: vbs-enclave/manual.md`.
