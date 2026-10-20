# Quickstart: RISC-V (RV64), `gramine-direct` in an emulated guest

The RISC-V port covers the direct PAL and is **experimental**: it is cross-built on an x86_64
machine and run in an Ubuntu 24.04 riscv64 guest under `qemu-system-riscv64` (full-system
emulation; user-mode emulation cannot run it, because it has no seccomp). It also builds and runs natively on a
RISC-V board (see [Native build on a board](#native-build-on-a-board)). Details: the RISC-V section of [INSTALL.md](../../INSTALL.md) and
`dev docs: riscv/PLAN.md`.

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

Not verified in the guest: other example suites.

## Native build on a board

On a riscv64 machine the same PAL builds and runs without the cross container and the guest. Verified on
2026-10-20 on an Orange Pi RV2 (Ky X1, 8 harts, 3.7 GiB RAM, Ubuntu 24.04) from a fresh clone of `main`:

    scripts/install.sh -j4                    # native build and install; keep -j low on a small board
    export GRAMINE_PREFIX="$HOME/.local/gramine" LOGDIR="$HOME/gramine-logs"
    test/direct/test-direct.sh                # PAL, LibOS and the example suites (or name suites: libos pal redis)

Results: the PAL regression passes 27 tests (9 skipped); the LibOS regression 190 (40 skipped, 3 failed); all 12
example suites of `test-direct.sh` pass (`helloworld`, `multi-thread`, `bash`, `sqlite`, `python`, `networks`,
`busybox`, `lighttpd`, `redis`, `nginx`, `memcached`, `rust`); `scripts/stress_rewrite.sh -n 20` passes all 11
scenarios in 20 of 20 cold runs; with `sys.rewrite_syscalls` a `getppid` takes 1.9 us against 7.4 us without it (3.9
times cheaper). The three LibOS failures:

- `test_054_large_mmap`: the board's RAM (the second `mmap` fails with `ENOMEM`).
- `test_2025_fork_reservation`: the child fails allocating an 8 GiB range (`ENOMEM`; the test asks for up to 16 GiB).
  Probably the board's RAM as well; not confirmed.
- `test_103_gettimeofday`: times out at the harness default of 10 s. Run directly it needs about 15 s for 4 threads
  of 1M calls and prints `TEST OK`; turning rewriting off does not change the time, so it is spent in the host clock path.

Notes for a board:

- The examples download their sources. On the tested board `/etc/resolv.conf` was written by Tailscale and listed only
  its own resolver, which does not answer public names, so every download failed (the empty file shows up as the hash
  mismatch `e3b0c442...`). Give the machine a working DNS resolver before running the examples.
- `memcached` needs `libevent-dev` (the workflow installs it with `apt`); `rust` needs `rustup` with a stable toolchain.
- Building the examples on the board takes a long time (`redis`, `lighttpd` and `busybox` together took 28 minutes).
