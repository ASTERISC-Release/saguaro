# Quickstart: RISC-V (RV64), `gramine-direct` in an emulated guest

The RISC-V port covers the direct PAL and is **experimental**: it is cross-built on an x86_64
machine and run in an Ubuntu 24.04 riscv64 guest under `qemu-system-riscv64` (full-system
emulation; user-mode emulation cannot run it, because it has no seccomp). It has not been run on
RISC-V hardware. Details: the RISC-V section of [INSTALL.md](../../INSTALL.md) and
[Documentation/devel/riscv/PLAN.md](../devel/riscv/PLAN.md).

Status (as in [README.md](../../README.md)): `direct` ⚠️; `vm` 🛫; graphics ❌.

Requirements: an x86_64 Linux machine with `podman` and about 2 GB of free disk for the guest; no
root and no KVM. Everything below runs in the cross-development container, `container/run.sh --target riscv64` (a
shell) or `container/run.sh --target riscv64 '<command>'` (one command; it exists for x86_64 hosts
only). The examples are run by `scripts/riscv64/run-examples.sh` in the guest, not by `./exec.sh`
(no riscv64 backend there).

## 1. Cross-build

    container/run.sh --target riscv64            # shell in the cross container; the rest runs inside
    scripts/install.sh --target riscv64 --prefix "$PWD/build-riscv64/prefix"

`--target riscv64` is the cross-build option of `scripts/install.sh`; the build directory defaults to
`build-riscv64` and no native install is overwritten.

The prefix lies inside the repository on purpose: the guest sees the repository at the same path,
so it runs the cross-built binaries directly.

## 2. Start the test guest

    scripts/riscv64/guest.sh start               # first time: downloads the Ubuntu image (about
                                                 # 800 MB) and installs packages; a few minutes
    scripts/riscv64/guest.sh ssh 'apt-get install -y build-essential'   # once: to build examples

The guest keeps its disk in `.riscv64-guest/` of the checkout (git-ignored). `guest.sh ssh`
opens a root shell in it, `guest.sh stop` stops it.

## 3. Hello world and the regression suites

The example applications are built natively in the guest:

    scripts/riscv64/run-examples.sh helloworld   # PASS helloworld

The PAL and LibOS suites need their test manifests rendered first:

    scripts/riscv64/render-test-manifests.sh pal
    scripts/riscv64/run-suite.sh pal             # about 15 s in the guest
    scripts/riscv64/render-test-manifests.sh libos
    scripts/riscv64/run-suite.sh libos           # about 2.5 min

    scripts/riscv64/guest.sh stop

`run-examples.sh` takes the suite names of `test/direct/test-direct.sh` (`bash`, `python`,
`sqlite`, `redis`, ...); several need more packages in the guest (the `direct-riscv64-emulated`
workflow in `.github/workflows/` lists the ones it installs).

## Troubleshooting

- `missing .../build-riscv64/run/pal/regression: run scripts/riscv64/render-test-manifests.sh pal first`: as it says.
- `make: command not found` from `run-examples.sh`: the guest has no compiler yet; install
  `build-essential` in it (step 2).
- `port 2222 is already in use`: another guest (or service) listens on the port in this network
  namespace; set `RISCV_GUEST_PORT`.

## Verified

2026-10-15, `main` at `9e28ea81`, x86_64 host (Arch Linux, rootless podman), fresh clone and
cross container: the cross-build and `guest.sh start` took about 5 minutes together (the guest
answered after about 150 s of its first boot); `run-suite.sh pal`: 27 passed, 9 skipped;
`run-suite.sh libos`: 187 passed, 38 skipped (2 min 22 s); `run-examples.sh helloworld`:
`PASS` after `build-essential` was installed in the guest (without it the build failed with
`make: command not found`).

Not verified: other example suites; anything on RISC-V hardware.
