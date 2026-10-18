# Example Integrations

This folder contains application integration examples. They are tested on
Ubuntu 24.04 (aarch64 and x86_64) with `gramine-direct`; on aarch64 a subset also runs under
`gramine-vm` (see `tools/run-vm-aarch64-examples.sh`).

For instructions how to build and run each application under Gramine, please
see the README.md in each subdirectory.

Please note that most of the examples use oversimplified configurations which
are not secure (e.g., they mount whole host directories into the application).
If you take these examples as templates for your own workloads, please inspect
and harden the configurations.

## Prerequisites

Install and enable Gramine first (`scripts/dependencies.sh`, `scripts/install.sh`
and `. ~/.local/gramine/env.sh`, see `INSTALL.md` in the repository root). Some examples need
extra host packages: each has an `install-prereqs.sh`, and
`install-all-prereqs.sh` runs all of them.

## Building Examples

All examples use simple Makefiles to build the application and generate its
Gramine manifest. Use one of these commands:
- `make`: create a no-debug-log manifest
- `make DEBUG=1`: create a debug-log manifest

Then run the application with `gramine-direct <application>`.

Use `make clean` to remove Gramine-generated artifacts and `make distclean` to
remove all build artifacts (if applicable).

## Testing

`test/test-direct.sh` (in the top-level `test/` directory) builds and runs most
of the examples as part of the test battery; `test/test-direct.sh --list` shows the
suites. `test/test-vm.sh examples` runs the aarch64 subset under `gramine-vm`. `unit_tests/` holds small test programs for LibOS and PAL behavior.

## Where the examples run

One row per example. The columns say where it was run; "-" means not filled in (not yet recorded
here). Only the VBS column has results so far: `make VBS=1` and `make VBS=1 check` on ARM64 and x64
Windows 11 on 2026-10-18, with the details in `Documentation/devel/vbs-enclave/TESTING.md`. "not run"
gives the reason (most examples have no VBS recipe).

| Example | Direct x86_64 | VM x86_64 | Direct aarch64 | VM aarch64 | SEV | VBS |
|---|---|---|---|---|---|---|
| `bash` | - | - | - | - | - | not run: no VBS recipe |
| `blender` | - | - | - | - | - | not run: no VBS recipe |
| `busybox` | - | - | - | - | - | pass (ARM64, x64) |
| `curl` | - | - | - | - | - | pass (ARM64, x64) |
| `gcc` | - | - | - | - | - | not run: no VBS recipe |
| `graphics` | - | - | - | - | - | not run: no VBS recipe |
| `helloworld` | - | - | - | - | - | pass (ARM64, x64) |
| `iperf` | - | - | - | - | - | pass (ARM64, x64) |
| `lighttpd` | - | - | - | - | - | pass (ARM64, x64) |
| `memcached` | - | - | - | - | - | pass (ARM64, x64) |
| `mongodb` | - | - | - | - | - | not run: no VBS recipe (x18 in mongod) |
| `nginx` | - | - | - | - | - | pass (ARM64, x64) |
| `nodejs` | - | - | - | - | - | fail on ARM64 (x18 in the node binary and libstdc++); not run on x64 (the recipe packs the linux-arm64 node) |
| `openjdk` | - | - | - | - | - | not run: no VBS recipe (the JVM needs executable memory) |
| `python` | - | - | - | - | - | ARM64: pass (hello and HTTP tests; NumPy/SciPy not packed); x64: fail at NumPy/SciPy (tests 3/4, 4/4), tests 1/4 and 2/4 pass |
| `pytorch` | - | - | - | - | - | not run: no VBS recipe |
| `r` | - | - | - | - | - | not run: no VBS recipe |
| `ra-tls-mbedtls` | - | - | - | - | - | not run: SGX attestation example |
| `ra-tls-nginx` | - | - | - | - | - | not run: SGX attestation example |
| `ra-tls-secret-prov` | - | - | - | - | - | not run: SGX attestation example |
| `redis` | - | - | - | - | - | pass (ARM64, x64) |
| `rust` | - | - | - | - | - | pass (ARM64, x64; needs rustup stable with rust-src) |
| `sqlite` | - | - | - | - | - | pass (ARM64, x64) |
| `unit_tests` | - | - | - | - | - | not run: LibOS/PAL test programs, no VBS recipe |
| `vbs/attestation` | - | - | - | - | - | pass (ARM64, x64) |
| `vbs/dynhello` | - | - | - | - | - | pass (ARM64, x64) |
| `vbs/helloworld` | - | - | - | - | - | pass (ARM64, x64) |
| `vbs/python` | - | - | - | - | - | pass (ARM64, x64) |
| `vbs/tcpserver` | - | - | - | - | - | pass (ARM64, x64) |
| `vbs/tls-echo` | - | - | - | - | - | pass (ARM64, x64) |
