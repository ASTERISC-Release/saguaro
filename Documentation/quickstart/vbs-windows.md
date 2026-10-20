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

This is the one guide that does not use the container of `container/run.sh`: the enclave is built
in WSL and run by Windows, with the Windows toolchain and a signing certificate, so a Linux
container on a Linux host cannot build or run it. The rest of the route is the same: the runtime is
installed with `scripts/install.sh --vbs`, and the examples run through `./exec.sh --vbs`.

## Requirements

- Windows 11 25H2 with **VBS enabled** and development environment setup (including enclave test signing
with a custom certificate and Visual Studio 2022 or later).
  - Refer to [the VBS manual](../vbs.md) for one-time machine setup details.
- **WSL2** with an Ubuntu 24.04 distribution.

# 1. Install

In WSL (Ubuntu 24.04), in a checkout of the repository. No sudo; nothing on Windows is changed:

    cd <repo-home>
    scripts/install.sh --vbs

This installs the missing build packages under `~/.local/share/gramine-vbs` (meson, ninja, clang, ...),
puts `gramine-vbs` in `~/.local/bin`, builds the vbs glibc, PAL and LibOS (a few minutes, once), and
checks the Windows side: it ends with `the Windows side is ready`, or lists the one-time Windows steps
that are missing. (The Windows PowerShell form `scripts\vbs\gramine-vbs.cmd setup` also checks both
sides.) The runtime is built once per checkout: `make VBS=1` no longer builds it and stops with an
install message when it is missing. Run `scripts/install.sh --vbs` again after changing the PAL or
the LibOS. `--vbs` builds nothing else: the other options of `scripts/install.sh` do not apply.

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
(the signed `helloworld.dll` in the folder `helloworld.vbs`, next to `helloworld.manifest.vbs`). `gramine-vbs helloworld` runs it,
like `gramine-direct helloworld`; if the package is missing or older than its manifest, it says to run
`make VBS=1` (it never builds):

    Hello, world from a VBS enclave (pid 1, 0 arguments)

`make VBS=1 check` does both and compares the output.

# 3. Example: Python

    cd CI-Examples/python
    ./exec.sh --vbs

By hand:

    make VBS=1
    gramine-vbs python

The first `make VBS=1` builds CPython 3.12.7 for the enclave in WSL (about 70 s, once) and packs the
interpreter with its standard library; `hello.py` prints the Python version, a SHA-256 and the
results of three threads.

Other examples with a `vbs.manifest.template`: `CI-Examples/redis`, `memcached`, `lighttpd`, `nginx`
and more (listed in [vbs.md](../vbs.md)); `./exec.sh --vbs` in their directory (or the same two commands by hand) builds and runs them. On ARM64 `busybox`, `sqlite`, `redis`, `memcached`, `iperf`, `curl`, `lighttpd`, `nginx` and `rust` pass; `nodejs` fails because `libstdc++` uses the reserved register x18 (results: [results-exec.md](../devel/ci-matrix/results-exec.md), 2026-10-20).

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
- `make VBS=1` stops with an install message: the runtime is missing; run `scripts/install.sh --vbs`.
- `./exec.sh --vbs` says the host cannot run `vbs`: it needs WSL on a Windows machine with VBS
  enclaves and the installed runtime (`build-vbs/runtime.stamp`).
- `gramine-vbs` reports a missing or older package: run `make VBS=1` in the example's folder.
- The application fails after launch because of process creation or JIT assumptions:
  VBS supports one process per enclave (no `fork`/`vfork`/new-process `execve`) and no run-time code generation.
- For an application of the user, write a `vbs.manifest.template` with `vbs.packed_files` (what goes into the image)
  and a Makefile that includes `scripts/vbs/Makefile.vbs`; start from `CI-Examples/helloworld` and
  [vbs.md](../vbs.md).

# 6. Verification status

Verified on 2026-10-20 on a Windows 11 ARM64 machine, in WSL (Ubuntu 24.04), with the runtime in
the checkout: `./exec.sh --vbs` for `helloworld`, `python` and the nine other examples listed in
section 3 (results and conditions in [results-exec.md](../devel/ci-matrix/results-exec.md)), and that
`make VBS=1` stops with the install message when the runtime is missing and builds nothing itself.
One early `helloworld` run failed in its check and passed on three re-runs; the cause is not known.

`CI-Examples/vbs/` has its own `exec.sh`, which runs `helloworld`, `dynhello`, `python`, `tcpserver`,
`attestation`, `tls-echo`, `jitprobe` and `rxprobe`; all eight pass on ARM64 (`jvmprobe`, an x64-only
research probe, is not run).

Not verified: `scripts/install.sh --vbs` itself as the step that builds the runtime of a fresh
checkout (the runtime of the tested checkout was built by the earlier `make VBS=1`) and the x64
Windows machine.
