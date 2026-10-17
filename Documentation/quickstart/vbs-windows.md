# Quickstart: Windows VBS enclaves (ARM64 Windows 11)

The `vbs` PAL runs an unmodified Linux AArch64 application inside a Windows
*virtualization-based security* (VBS) enclave. Because a VBS enclave cannot make memory executable
at run time, the library OS, a glibc, the application and its files are packed into a signed
enclave image when it is built. `gramine-vbs.cmd` does the whole build on the Windows machine,
with its Linux half in WSL2. The manual: [Documentation/vbs.md](../vbs.md).

Status (as in [README.md](../../README.md)): AArch64 (ARM64 Windows 11) ⚠️ experimental;
x86_64 ❌.

## Prerequisites

Each changes the machine and is done once by an administrator (details and commands in
[Documentation/vbs.md](../vbs.md), Installation):

1.  ARM64 Windows 11 with **VBS running** (Core isolation, memory integrity; check that
    `Win32_DeviceGuard` reports `VirtualizationBasedSecurityStatus` `2`).
2.  **Test signing** on (`bcdedit /set testsigning on`, restart).
3.  A **signing certificate** in the current user's store with the code-signing, enclave-signing and
    enclave-author EKUs (default subject `CN=TheDefaultTestEnclaveCertName`).
4.  **WSL2** with an Ubuntu 24.04 distribution (aarch64 on ARM64 Windows).
5.  **Visual Studio** 2022 or later with the C++ desktop workload, the ARM64 build tools and the
    Windows SDK 10.0.26100 or later.

## 1. Check the machine and install the build tools

In PowerShell, in a checkout of the repository (any folder; Git's default line ends are fine):

    scripts\vbs\gramine-vbs.cmd setup            # checks Windows and WSL, prints what is missing
    scripts\vbs\gramine-vbs.cmd setup -Install   # if build tools are missing: user-level, no sudo

`setup` ends with `everything needed is present` when the machine is ready. `-Install` puts
the Ubuntu packages it needs (meson, ninja, clang, ...) into `~/.local/share/gramine-vbs` of the
WSL distribution; `-Apt` installs them with `sudo apt-get` instead.

## 2. Hello world

    scripts\vbs\gramine-vbs.cmd run CI-Examples\vbs\helloworld\helloworld.manifest

The first run copies the checkout into WSL, builds the vbs glibc, the PAL and the library OS
(about 3.5 minutes), compiles and packs the program, then links, signs and runs the enclave:

    Hello, world from a VBS enclave (pid 1, 0 arguments)

Later runs start the signed image in about 2 seconds, and rebuild only when a file changed.
`build` and `run` can also be separate steps; the image and the host program land in
`helloworld.vbs\` next to the manifest:

    scripts\vbs\gramine-vbs.cmd build CI-Examples\vbs\helloworld\helloworld.manifest
    scripts\vbs\gramine-vbs.cmd run   CI-Examples\vbs\helloworld\helloworld.vbs

## 3. Python

    scripts\vbs\gramine-vbs.cmd run CI-Examples\vbs\python\python.manifest

The first run builds CPython 3.12.7 for the enclave in WSL (about 70 s, once) and packs the
interpreter with its standard library; `hello.py` prints the Python version, a SHA-256 and the
results of three threads. Other examples in `CI-Examples\vbs`: `dynhello` (a dynamically linked
program), `tcpserver` (sockets through the host), `attestation`.

Your own application needs a manifest with `vbs.packed_files` (what goes into the image) and
`vbs.build` (how it is built in WSL); start from the examples and [vbs.md](../vbs.md).

## Limits

- One process per enclave: no `fork`, `vfork` or `execve` into a new process.
- No run-time code generation (no JIT): all code must be packed into the image when it is built.
- Debug enclaves signed with a test certificate; production signing has not been tried.
- A thread inside the enclave sees a signal only at its next system call.

## Attestation

`/dev/attestation` inside the enclave returns a VBS attestation report for 64 bytes of user
data, and `gramine-vbs.cmd verify` checks one on the same machine:

    cd CI-Examples\vbs\attestation
    ..\..\..\scripts\vbs\gramine-vbs.cmd run attest.manifest -- <64 hex digits>
    ..\..\..\scripts\vbs\gramine-vbs.cmd verify attest.vbs -Report attest.vbs\root\data\out\report.bin -- --user-data <64 hex digits> --same-image --min-svn 1

The check is local to this machine; what it establishes and what it does not is in
[vbs.md](../vbs.md), Attestation.

## Verified

2026-10-15, `main` at `9e28ea81`, on ARM64 Windows 11 Pro 25H2 (build 26200) with VBS
running, an existing test certificate and an existing Ubuntu 24.04 WSL distribution, from a fresh
clone, in Windows PowerShell 5.1: `setup` (`everything needed is present`); `setup -Install`
(nothing to install: the tools were already in the distribution); hello world, first run 214 s,
second 2.1 s; `build` then `run` of the `.vbs` folder (20 s, 2.9 s); Python, first run 103 s
(CPython build 71 s), second 2.9 s, output `Python 3.12.7 on Linux aarch64`; the attestation
example and its `verify`: `result: VERIFIED (locally, this machine)`.

Not verified: the prerequisites (they were already in place on the test machine, so their
commands were not run), `-Install` downloading packages, `-Apt`, `cmd` instead of
PowerShell, and a machine set up from scratch.
