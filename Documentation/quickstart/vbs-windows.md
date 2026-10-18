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

# 1. Build

In PowerShell, in a checkout of the repository (any folder; Git's default line ends are fine):

    scripts\vbs\gramine-vbs.cmd setup            # checks and installs missing WSL packages user-level

`setup` ends with `everything needed is present` when the machine is ready. It puts missing Ubuntu
build packages (meson, ninja, clang, ...) under `~/.local/share/gramine-vbs` in WSL, without sudo.

Optionally, `setup-system` installs missing packages systemwide with `sudo apt-get`.

    scripts\vbs\gramine-vbs.cmd setup-system     # installs missing WSL packages with sudo apt-get

Next step is to copy the checkout into WSL, build the vbs glibc, PAL and LibOS (~3-5 minutes).

    scripts\vbs\gramine-vbs.cmd build

# 2. Example: Hello World

Package the manifest explicitly, then run the signed image:

    scripts\vbs\gramine-vbs.cmd package CI-Examples\vbs\helloworld\helloworld.manifest
    scripts\vbs\gramine-vbs.cmd run CI-Examples\vbs\helloworld\helloworld.vbs

`package` compiles and packs the program, then links and signs the enclave. `run` only launches an
existing package:

    Hello, world from a VBS enclave (pid 1, 0 arguments)

If the package is missing or stale, run `package` explicitly; `run` does not rebuild it.
The image and host program land in `helloworld.vbs\` next to the manifest.

# 3. Example: Python

    scripts\vbs\gramine-vbs.cmd package CI-Examples\vbs\python\python.manifest
    scripts\vbs\gramine-vbs.cmd run CI-Examples\vbs\python\python.manifest

The first `package` builds CPython 3.12.7 for the enclave in WSL (about 70 s, once) and packs the
interpreter with its standard library; `hello.py` prints the Python version, a SHA-256 and the
results of three threads.

Other examples in `CI-Examples\vbs`: `dynhello` (a dynamically linked program), `tcpserver`
(sockets through the host), and `attestation`.

# 4. Attestation

`/dev/attestation` inside the enclave returns a VBS attestation report for 64 bytes of user
data, and `gramine-vbs.cmd verify` checks one on the same machine:

    cd CI-Examples\vbs\attestation
    ..\..\..\scripts\vbs\gramine-vbs.cmd package attest.manifest
    ..\..\..\scripts\vbs\gramine-vbs.cmd run attest.manifest -- <64 hex digits>
    ..\..\..\scripts\vbs\gramine-vbs.cmd verify attest.vbs -Report attest.vbs\root\data\out\report.bin -- --user-data <64 hex digits> --same-image --min-svn 1

The check is local to this machine; what it establishes and what it does not is in
[vbs.md](../vbs.md), Attestation.

# 5. Troubleshooting

- A setup prerequisite is missing: run `scripts\vbs\gramine-vbs.cmd setup` and fix the first failed check.
- If `run` reports a missing or stale package, run `build` if the runtime is not built yet, then
  `package <manifest>`; `run` only launches an available package.
- Your application fails after launch because of process creation or JIT assumptions:
  VBS supports one process per enclave (no `fork`/`vfork`/new-process `execve`) and no run-time code generation.
- For your own app, make sure the manifest includes `vbs.packed_files` (what goes into the image)
  and `vbs.build` (how files are built in WSL); start from the examples and [vbs.md](../vbs.md).
