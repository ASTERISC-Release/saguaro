# Quickstart: Intel SGX and TDX

This tree keeps the confidential-computing back ends of upstream [gramine-tdx](https://github.com/gramineproject/gramine-tdx) for Intel CPUs: `gramine-sgx` (the application
in an SGX enclave) and `gramine-tdx` (the `vm` unikernel in a TDX trust domain). The examples that run on
SEV also run on TDX.

Status (as in [README.md](../../README.md)): Intel x86_64 ⚠️ for SGX and for TDX; AMD64, AArch64 and RISC-V ➖.

As in the other guides, the build runs in the container of `container/run.sh` (see
[direct.md](direct.md), section 1); the commands below run in its shell, in the repository, unless
they say that they run on the host.

## SGX

`gramine-sgx` runs an unmodified application in an Intel SGX enclave. The enclave is built and
signed from the application's manifest; the Saguaro runtime, the library OS and the application are
measured into it.

### Requirements

- An Intel CPU with SGX and flexible launch control: the `sgx` and `sgx_lc` flags in `/proc/cpuinfo`,
  SGX enabled in the firmware, and a Linux kernel (5.11 or newer) with the in-kernel driver, which
  provides `/dev/sgx_enclave` (and `/dev/sgx_provision`).
- `/dev/sgx_enclave` readable and writable by the user: add the user to the `sgx` group on the host.
- `podman` and the `crun` runtime for the container of `container/run.sh` (`uidmap`, `slirp4netns` and
  `fuse-overlayfs` for rootless containers). Nothing else is installed on the host.
- No other host setting is needed. In particular, `vm.mmap_min_addr` stays at the default of the
  distribution (the loader's message `You may need to set sysctl vm.mmap_min_addr to zero` is
  misleading, see Troubleshooting), and the examples run without the AESM service. Remote
  attestation needs the AESM service, a PCCS and `sgx.remote_attestation` in the manifest; that is not covered here.

### 1. Prepare the host (once)

On the host (Ubuntu 24.04; these steps need `sudo`):

    sudo apt-get update
    sudo apt-get install -y podman crun uidmap slirp4netns fuse-overlayfs
    sudo usermod -aG sgx "$USER"

Log in again so that the group applies, then check:

    id -nG                             # contains sgx
    ls -l /dev/sgx_enclave             # crw-rw---- root sgx
    podman info --format '{{.Host.Security.Rootless}} {{.Host.OCIRuntime.Name}}'   # true crun

### 2. Build and install

    cd <repo-home>                     # a checkout in a folder saguaro-<name>/saguaro
    container/run.sh                   # shell in the container; it gets /dev/sgx_enclave
    scripts/install.sh --sgx           # builds in build-sgx, installs to ~/.local/saguaro-sgx
    . ~/.local/saguaro-sgx/env.sh
    is-sgx-available

`container/run.sh` passes `/dev/sgx_enclave` and `/dev/sgx_provision` into the container, bound with
the `exec` option (Saguaro maps the enclave device with `PROT_EXEC`). `scripts/install.sh --sgx` has its own build
directory and prefix, so it does not touch a direct install. On a host with SGX2 and flexible launch
control, `is-sgx-available` prints, among others:

    SGX supported by CPU: true
    SGX1 (ECREATE, EENTER, ...): true
    SGX2 (EAUG, EACCEPT, EMODPR, ...): true
    Flexible Launch Control (IA32_SGXPUBKEYHASH{0..3} MSRs): true
    SGX driver loaded: true

### 3. Example: Hello World

    cd CI-Examples/helloworld
    ./exec.sh --sgx

`exec.sh` creates the enclave signing key `~/.config/gramine/enclave-key.pem` once (the equivalent of
`gramine-sgx-gen-private-key`), runs `make SGX=1` (renders the manifest and signs it: `helloworld.manifest.sgx`
and `helloworld.sig`, with the enclave measurement printed) and runs `gramine-sgx ./helloworld`. The output ends with:

    Hello, world
    [PASS] helloworld sgx x86_64 (...)

The steps by hand:

    gramine-sgx-gen-private-key        # once
    make SGX=1
    gramine-sgx helloworld

The manifest of this example sets `sgx.enclave_size = "1G"`: the program maps 512 MiB, and the default
enclave size is 256M.

### 4. The other examples

Every example whose Makefile has an `SGX=1` build runs with `./exec.sh --sgx` in its directory.
`bash`, `busybox`, `curl`, `gcc`, `iperf`, `lighttpd`, `memcached`, `mongodb`, `nginx`, `nodejs`,
`openjdk`, `python`, `r`, `redis`, `rust`, `sqlite`, `blender` and `pytorch` pass
([RESULTS.md](../../CI-Examples/RESULTS.md)). `python` passes
without the quote step (it needs remote attestation, `RA_TYPE=dcap`). `unit_tests` has an SGX build for
`mremap` only, and that test fails: the tail of a shrunk mapping stays readable in an enclave.
The enclave sizes are set by each manifest (`mongodb` 8G, `openjdk` 16G, `pytorch` 4G).

In a fresh container, `blender`, `mongodb`, `nodejs`, `openjdk` and `r` first stop with "cannot run here: missing on the host"; run `CI-Examples/install-all-prereqs.sh` once (or the `install-prereqs.sh` of that example, which the message names) and repeat the command.

### 5. Troubleshooting

- `./exec.sh --sgx` says `no SGX device (/dev/sgx_enclave)`: the host has no SGX driver, or the
  container was created without the device (before the user was in the `sgx` group, or with an older `container/run.sh`).
  Recreate the container with `container/run.sh down` on the host, then repeat section 2 (the install inside it is discarded).
- `no read/write access to /dev/sgx_enclave (group sgx)`: the user is not in the `sgx` group (section 1).
- `error: Permission denied on mapping enclave. You may need to set sysctl vm.mmap_min_addr to zero`,
  `Allocation of EPC memory failed: Operation not permitted (EPERM)`: Saguaro's message for any `EPERM`
  of the `PROT_EXEC` mapping of `/dev/sgx_enclave`. In a rootless container it was the `noexec` mount that
  `--device` gives the node; the container of this `container/run.sh` binds it with `exec`. Check
  `findmnt -T /dev/sgx_enclave` in the container (no `noexec`), and recreate a container that was created earlier.
- `mmap ...: Cannot allocate memory` or an allocation failure inside the application: the enclave is
  too small; raise `sgx.enclave_size` in the manifest (a power of two).
- `There are no available TCS pages left for a new thread`: raise `sgx.max_threads` in the manifest.
- `Cannot find /dev/attestation/quote`: the manifest has no remote attestation (`sgx.remote_attestation`);
  see Requirements.
- The `make SGX=1` step needs the protobuf-c tools (`protobuf-c-compiler libprotobuf-c-dev`), which the
  container image has.

Reference: [attestation.md](../attestation.md), the `sgx.*` keys of [manifest-syntax.md](../manifest-syntax.md), and [gramine.readthedocs.io](https://gramine.readthedocs.io).

## TDX

Needs an Intel CPU and host kernel with TDX, a QEMU with TDX support and the TD-shim firmware, as
described by the upstream gramine-tdx project. The build is `scripts/install.sh --vm --tdx` (in the container); the
launcher is `gramine-tdx` (the TDX variant of `gramine-vm`, `tools/gramine-vm.in`).

The install step links the PAL with TD-shim: it runs `cargo run -p td-shim-tools` in
`$HOME/td-shim`, so that checkout (built as upstream gramine-tdx describes) must exist, or the
install stops there after everything has compiled.

The examples that run on SEV also run on TDX. `exec.sh` has no TDX backend. CI compiles the `tdx` PAL
(the `tdx-build` job of `direct-x86_64.yml`).

## Not covered

SGX remote attestation (the AESM service, a PCCS, `RA_TYPE=dcap`), the `CI-Examples/ra-tls/` examples, and
SGX EDMM beyond the `mremap` test.
