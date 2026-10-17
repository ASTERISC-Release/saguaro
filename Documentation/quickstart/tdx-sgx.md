# Quickstart: Intel TDX and SGX (inherited, not verified here)

This tree keeps the confidential-computing back ends of upstream [gramine-tdx](https://github.com/gramineproject/gramine-tdx) for Intel CPUs: `gramine-sgx` (the application
in an SGX enclave) and `gramine-tdx` (the `vm` unikernel in a TDX trust domain). **Neither has
been run on SGX or TDX hardware as part of this tree's work**; use the upstream documentation and
treat this page as pointers.

Status (as in [README.md](../../README.md)): Intel x86_64 ✅ (inherited); AMD64, AArch64 and
RISC-V ➖.

## SGX

Needs an Intel CPU with SGX (FLC), the in-kernel SGX driver (`/dev/sgx_enclave`) and, for remote
attestation, Intel's DCAP or EPID services. The SGX build also needs the protobuf-c tools, which
`scripts/dependencies.sh` does not install (`protobuf-c-compiler libprotobuf-c-dev`):

    scripts/install.sh --sgx --prefix ~/.local/gramine-sgx --build-dir build-sgx
    . ~/.local/gramine-sgx/env.sh
    is-sgx-available                   # what the CPU and kernel offer
    gramine-sgx-gen-private-key        # once: the enclave signing key
    cd CI-Examples/helloworld
    make SGX=1                         # renders, then signs: helloworld.manifest.sgx, helloworld.sig
    gramine-sgx helloworld

Reference: [sgx-intro.md](../sgx-intro.md), [sgx-setup.md](../sgx-setup.md),
[attestation.md](../attestation.md), the `sgx.*` keys of [manifest-syntax.md](../manifest-syntax.md), and [gramine.readthedocs.io](https://gramine.readthedocs.io).

## TDX

Needs an Intel CPU and host kernel with TDX, a QEMU with TDX support and the TD-shim firmware, as
described by the upstream gramine-tdx project. The build is `scripts/install.sh --vm --tdx`; the
launcher is `gramine-tdx` (the TDX variant of `gramine-vm`, `tools/gramine-vm.in`).

The install step links the PAL with TD-shim: it runs `cargo run -p td-shim-tools` in
`$HOME/td-shim`, so that checkout (built as upstream gramine-tdx describes) must exist, or the
install stops there after everything has compiled.

Fixed issue (2026-10-15): until the `dev-tdx-build` fix the `tdx` PAL did not compile
(`pal/src/host/vm-common/pal_common_sockets.c`: `invalid application of 'sizeof' to incomplete type 'struct udp_rx_ring'`): the network code shared by the vm PALs used a structure that only the
`vm` PAL's host header defined. The structure is now in the shared `vm-common` header, and the
`tdx-build` job of `direct-x86_64.yml` compiles the `tdx` PAL on every manual run.

## Verified

2026-10-15, `main` at `9e28ea81`, in the x86_64 container on a machine **without** SGX or TDX:
after installing `protobuf-c-compiler libprotobuf-c-dev protobuf-compiler python3-protobuf` in
the container, `scripts/install.sh --sgx ...` built and installed; `is-sgx-available` printed
`SGX supported by CPU: false`; `gramine-sgx-gen-private-key` and `make SGX=1` produced the
signed `helloworld.manifest.sgx` and `helloworld.sig`; `gramine-sgx helloworld` stopped with
`Cannot open SGX driver device`, as expected without SGX. `scripts/install.sh --tdx` and
`--vm --tdx` failed with the build error above.

2026-10-15, after the fix (same container, no TDX): `scripts/install.sh --tdx` and `--vm --tdx`
compile everything, including the `tdx` PAL (`pal/src/host/tdx/pal`), and stop at the TD-shim
link step of the install (no `$HOME/td-shim` here).

Not verified: running anything under SGX or TDX, attestation, the TD-shim link step, and the TDX
QEMU and firmware.
