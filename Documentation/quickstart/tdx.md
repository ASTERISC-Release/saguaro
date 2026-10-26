# Quickstart: Intel Trust Domain eXtensions (`saguaro-tdx`)

`saguaro-tdx` runs a Saguaro micro-VM as an Intel TDX trust domain (TD). TDX protects the memory
and registers of the TD from the host. The TD boots from the [td-shim](https://github.com/confidential-containers/td-shim)
firmware, which loads the TDX PAL as its payload. See the [VM quickstart](vm.md).

## Requirements

- `podman` for the container of `container/run.sh`. The TDX tools are built inside the container;
  nothing is installed on the host.

### Hardware

All the requirements from [VM quickstart](vm.md) apply, and:

- An Intel host with TDX enabled in the firmware and a host kernel with TDX support
  (`cat /sys/module/kvm_intel/parameters/tdx` prints `Y`).
  - Refer to the upstream [gramine-tdx](https://github.com/gramineproject/gramine-tdx) project for details on enabling TDX.
- A QEMU with TDX support (`tdx-guest`): `QEMU_PATH`, or `qemu-system-x86_64` from `PATH`.

### Emulation (Optional)

We have also designed a functional emulation of TDX (for testing/development). For this, you need:

- Our fork of QEMU--[`qemu-tdx-tcg`](https://github.com/adilahmad17/qemu-tdx-tcg)--from the `dev-tdx` branch,
  pinned as the git submodule `vmm/tdx-tcg` (a git clone of this repository is needed; a ZIP download does not
  contain it).
  - Fetch and build it inside the podman container, into `vmm/tdx-tcg/prefix`:

        container/run.sh 'scripts/qemu.sh build tdx-tcg'

# 1. Build and install

The container is opened as in [direct.md](direct.md), section 1. td-shim is built once into
`$HOME/td-shim` (the install links the PAL with it), then Saguaro with the TDX PAL:

    cd <repo-home>
    container/run.sh "git clone https://github.com/confidential-containers/td-shim ~/td-shim && git -C ~/td-shim checkout 54a862b249a62248b726cd3dc4a96421fdc36d66"
    container/run.sh 'apt-get update && apt-get install -y clang llvm rustup'   # td-shim needs them; the image has nasm
    container/run.sh 'PATH=/usr/bin:$PATH scripts/td-shim-build.sh'
    container/run.sh "scripts/install.sh --sgx --tdx --graphics disabled --prefix ~/.local/saguaro-tdx"   # SGX tools + TDX PAL + saguaro-tdx

Note the following: 

- The PAL is linked without a signature, so `scripts/td-shim-build.sh` builds td-shim without its
`secure-boot` feature.
- `--sgx` is required: the TDX PAL reads the signed manifest `<app>.manifest.sgx`
that `make SGX=1` produces.
- `--prefix` is the default prefix of `exec.sh` and `test/quick.sh` for `tdx`
(`SAGUARO_TDX_PREFIX`); without it `--sgx` installs into `~/.local/saguaro-sgx`. 

# 2. Example: Hello World

In the container shell, `exec.sh` runs the example under TDX (`--tdx`):

    cd CI-Examples/helloworld
    ./exec.sh --tdx

If you are using **emulation** mode, export the following environment variables first, in the container
shell at the root of the checkout:

    export SAGUARO_ACCEL=tcg QEMU_PATH=$(scripts/qemu.sh path tdx-tcg)
    cd CI-Examples/helloworld
    ./exec.sh --tdx            # every example's exec.sh works

The script creates the enclave signing key `~/.config/saguaro/enclave-key.pem` once, runs `make SGX=1`
(renders and signs the manifest) and runs `saguaro-tdx`. The output contains:

    Hello, world
    [PASS] helloworld tdx x86_64 (...)

The steps by hand (`container/run.sh` does not put this install on `PATH`):

    . ~/.local/saguaro-tdx/env.sh
    cd CI-Examples/helloworld
    saguaro-sgx-gen-private-key        # once; it refuses to overwrite the key `exec.sh` made
    make SGX=1
    saguaro-tdx helloworld

Note the following:

- The guest RAM is the `sgx.enclave_size` of the manifest, at least 4G (the memory layout has two
  NUMA nodes of 2G). Without a size in the manifest, `SAGUARO_RAM_SIZE` applies (default 8G).
- Set `SAGUARO_CPU_NUM` as for `saguaro-vm`.
- `./exec.sh --tdx` passes for the examples `helloworld`, `bash`, `sqlite`, `python`, `busybox`, `memcached`, `redis`,
  `lighttpd`, `rust` and `nginx` under the emulation. The tests of `unit_tests` that need an SGX build are skipped.

# 3. Regression Tests

    test/quick.sh tdx                             # without SAGUARO_ACCEL=tcg: compiles the TDX PAL
    SAGUARO_ACCEL=tcg test/quick.sh tdx --tier smoke        # under the emulation (QEMU_PATH as above): about 20 s
    SAGUARO_ACCEL=tcg test/quick.sh tdx                     # the regression tier
    SAGUARO_ACCEL=tcg test/quick.sh tdx --tier extended     # the full LibOS suite, sweeps, stress tests: hours

Under the emulation the smoke tier runs the install check, helloworld, `unit_tests` and the LibOS smoke tests. The
regression tier adds the LibOS core list (127 tests, the same as on every variant) at 2 vCPUs. The tests that fork
are expected failures (`test/quick/expected-failures.toml`): the TDX PAL has no process creation. The results say
`tdx-emu`. The scripts underneath:

    CI-Examples/common_tools/sev-emu-check.sh tdx     # the preflight: is QEMU_PATH the QEMU of vmm/tdx-tcg?

The workflow `.github/workflows/tdx-emulated-x86_64.yml` runs the emulated tiers on a hosted runner, nightly and on
demand; the `tdx-build` job of `direct-x86_64.yml` compiles the TDX PAL.

# 4. Troubleshooting

- `exec.sh` reports that `tdx` cannot run: it names the reason. `no TDX host (kvm_intel tdx is not Y)`: the host has no
  TDX; use the emulation (`SAGUARO_ACCEL=tcg`, `QEMU_PATH`).
- `has no 'tdx-emu' object`: `QEMU_PATH` is not the QEMU of `vmm/tdx-tcg` (`scripts/qemu.sh path tdx-tcg`).
- The install stops at the link step: `$HOME/td-shim` is missing or not built (section 1).
- `saguaro-sgx-sign not installed`: the install lacks `--sgx`; repeat it with `--sgx --tdx`.
- td-shim panics with `Secure Boot: Cannot read verify header`: it was built with its default features; build it with
  `scripts/td-shim-build.sh`.
- `saguaro-tdx not installed in ~/.local/saguaro-vm`: the container loaded the `vm` install and set `SAGUARO_PREFIX`; in a
  container that holds both the SEV and the TDX install, run `unset SAGUARO_PREFIX` first.
- Two VM launchers (`saguaro-vm`, `saguaro-sev`, `saguaro-tdx`) at the same time in one container collide on the vsock
  relay: run one at a time. A TDX run leaves signed `.manifest.sgx` files in `libos/test/regression` that make
  `saguaro-sev` use 4G of RAM and fail with `Cannot initialize the multiprocess frame pool`: remove them between variants.
