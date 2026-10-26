# Installing

Short walk-throughs per platform (direct, vm, graphics, RISC-V, Windows VBS, TDX/SGX) are
in the [quickstart guides](Documentation/quickstart/README.md); this page is the complete
reference.

This tree builds and runs on Debian/Ubuntu (24.04 is the tested baseline) on **x86_64** and
**aarch64**, in two ways (the experimental **riscv64** port of the direct PAL is cross-built instead;
see [RISC-V (cross-build and test guest)](#risc-v-cross-build-and-test-guest)):

- `saguaro-direct`, the *direct* PAL: the library OS runs as an ordinary Linux process. It needs
  only the build tools and works anywhere, including in a container.
- `saguaro-vm`, the *vm* PAL: the library OS and a small kernel boot as a unikernel in a QEMU guest.
  It needs a **native** machine of the same architecture with KVM (`/dev/kvm`), a stock
  `qemu-system-*`, `virtiofsd` and, on aarch64, `dtc`. On aarch64 it is experimental.

Install on the host ([Installing on the host](#installing-on-the-host)) or use the supplied containers
([Installing in a container](#installing-in-a-container)). The container route is the quickest way to get the `vm` PAL: the
image already has QEMU, `virtiofsd` and the toolchain.

## Installing on the host

### Direct PAL

From the repository root:

    scripts/dependencies.sh           # host packages (apt, uses sudo); --build-only for just the build
    scripts/install.sh                # build and install into ~/.local/saguaro
    . ~/.local/saguaro/env.sh         # put the install on PATH / PYTHONPATH (this shell)

`scripts/install.sh` is safe to re-run. The main options (`--help` lists all):

|  |  |
|----|----|
| `--prefix DIR` | install prefix, no root needed; default `~/.local/saguaro`, `~/.local/saguaro-vm` with `--vm` or `--sev`, `~/.local/saguaro-sgx` with `--sgx` (without `--vm`), `~/.local/saguaro-riscv64` with `--target riscv64` |
| `--build-dir DIR` | meson build directory; default `build-<arch>`, `build-vm-<arch>` with `--vm`, `build-sgx` with `--sgx` (without `--vm`), `build-riscv64` with `--target riscv64` |
| `--vm`, `--tdx`, `--sgx` | also build that PAL (the direct PAL is always built, except by `--vm` on aarch64) |
| `--sev` | `--vm` plus the AMD SEV build of the vm PAL and `saguaro-sev` (x86_64) |
| `--vbs` | the Windows VBS tooling, in WSL on the Windows machine (`scripts/vbs/install-vbs.sh`: `saguaro-vbs` in `~/.local/bin`, the vbs runtime in `build-vbs`); nothing else is built |
| `--graphics disabled` | leave out the Tirith graphics host-redirection paths |
| `--libc glibc|none|musl` | libc of the runtime directory; see [Which libc](#which-libc) |
| `--target riscv64` | cross-build (see [RISC-V (cross-build and test guest)](#risc-v-cross-build-and-test-guest)) |
| `--deps` | run `scripts/dependencies.sh --build-only` first |
| `--add-to-profile` | source `<prefix>/env.sh` from `~/.bashrc` |
| `-j N` | parallel build jobs (default: all cores) |
| `--run-tests [suite...]` | run `test/direct/test-direct.sh` (suites `pal` and `libos`) afterwards (naming suites implies it) |

Check the install and run an example:

    test/quick.sh direct --tier smoke        # install check, helloworld, unit_tests, two LibOS tests (about 20 s)
    test/quick.sh direct                     # the regression tier: PAL suite, LibOS core list, nine examples (under a minute once the examples are built)
    test/direct/test-direct.sh libos         # the full PAL / LibOS suites alone (`pal`, `libos`; `--list` shows them)

The tiers (`smoke`, `regression`, `extended`), their output files and the exit status are described in
[test/README.md](test/README.md).

    cd CI-Examples/helloworld
    make
    saguaro-direct helloworld

### Virtual-machine PAL (`saguaro-vm`)

Install the QEMU side, then build with `--vm`.

aarch64:

    sudo apt install qemu-system-arm virtiofsd device-tree-compiler    # stock Ubuntu QEMU (8.2.x)
    scripts/dependencies.sh --build-only
    scripts/install.sh --vm           # into ~/.local/saguaro-vm, build dir build-vm-aarch64
    . ~/.local/saguaro-vm/env.sh
    test/quick.sh vm                     # LibOS core list under saguaro-vm, examples, network suites (2 vCPUs)
    test/vm/test-vm.sh                   # the whole LibOS suite under saguaro-vm and the loopback suite

On aarch64 `--vm` builds *only* the vm PAL (no direct PAL, SGX or TDX), builds without graphics (the
redirection needs the host agent of the graphics QEMU, which does not exist for aarch64) and builds the test
programs as PIE, because the guest maps RAM at `0x40000000`. Use stock distribution QEMU; the
QEMU backends under `vmm/` are not needed and do not build for aarch64. The VM PAL runs multi-process
applications (`fork`, `vfork`, `execve`) by default, as on x86_64 (`sys.experimental__multiprocess`, see
[quickstart/vm.md](Documentation/quickstart/vm.md)). Guest TCP and UDP go through a vsock relay that the launcher starts,
which needs `/dev/vhost-vsock` (module `vhost_vsock`) to be readable and writable by your user, as
`/dev/kvm` is. See `dev docs: vm.md` for the exact limits.

x86_64:

    sudo apt install qemu-system-x86 virtiofsd lsof python3-psutil
    scripts/install.sh --vm --graphics disabled [--tdx] [--sgx]
    sudo ln -sf /usr/libexec/virtiofsd /usr/local/bin/virtiofsd    # if `command -v virtiofsd` finds nothing

The x86_64 vm PAL is inherited from upstream gramine-tdx. `saguaro-vm` runs the QEMU named by
`QEMU_PATH`, otherwise a stock `qemu-system-x86_64` from `PATH`. The graphics redirection and the
emulated SEV and TDX need a QEMU backend of `vmm/` (git submodules): `scripts/qemu.sh build graphics`
(or `sev-tcg`, `tdx-tcg`) fetches and builds one, and `QEMU_PATH=$(scripts/qemu.sh path <name>)` selects
it; for graphics see [graphics-vm.md](Documentation/quickstart/graphics-vm.md). With graphics enabled (the default), `--vm` also builds
`libgraphics.so` and needs the Mesa, X and SDL2 development packages; `--graphics disabled` builds
without them.

Running an application under the VM PAL is described in `dev docs: vm.md`: the manifest's
loader entrypoint must name the VM `libsysdb.so` (`make` against the VM install does this), the
binary must be a PIE on aarch64, and you start it with `saguaro-vm <application>`. A kernel panic or
guest exit is reported as `[ VM exited with code N ]`.

### Which libc

By default no glibc is built (`-Dlibc=none`, a much shorter build) for `--vm` builds and for direct
aarch64 and riscv64 builds, unless `--sgx` or `--tdx` is set: applications run on the host's glibc.
`saguaro.runtimedir()` in the manifests then names the host directory that holds `libc.so.6` and the
dynamic loader, found when the manifest is rendered; `SAGUARO_HOST_LIBC_DIR` overrides the search (the
RISC-V test manifests, rendered in a container, set it to the guest's `/lib/riscv64-linux-gnu`).

`--libc glibc` builds and uses the bundled glibc instead; SGX needs it. An x86_64 direct build without
`--vm` keeps it by default, because the patched glibc avoids a seccomp trap on every system call
(about 0.85 us). A `--vm` build on x86_64 therefore has a slower direct PAL: build without `--vm`, in
a build directory of its own, for the patched glibc. On aarch64 the direct PAL needs no patched libc.

## Installing in a container

`container/run.sh` is the single entry point. It detects the machine's architecture, builds the
matching image if it is missing or its Containerfile changed, creates a long-lived container and opens a
shell in it. The repository is mounted at `/root/<checkout folder>` (`/root/saguaro` for a
checkout named `saguaro`, as `git clone git@github.com:ASTERISC-Release/saguaro.git` makes); builds
and install prefixes live in the container's home. You need `podman`, and for `saguaro-vm` a native
host of the same architecture with `/dev/kvm`.

    container/run.sh                      # shell in the container for this machine
    container/run.sh '<command>'          # one-shot command (no tty)
    container/run.sh --rebuild            # force an image rebuild and container recreation
    container/run.sh down                 # stop and remove the container (the image is kept)

Typical session (on x86_64 the container is `saguaro-vm-x86_64`):

    container/run.sh                              # shell inside
    scripts/install.sh pal libos                  # direct PAL: build, install, run the PAL + LibOS suites
    scripts/install.sh --vm libos loopback        # aarch64 vm PAL: build, install, run the vm test battery
    scripts/install.sh --vm && test/quick.sh vm   # x86_64 vm PAL: build, install, the regression tier in the VM

or as a one-shot command from the host: `container/run.sh 'scripts/install.sh --vm && test/quick.sh vm'`.
(On x86_64, test suites named after `--vm` go to `test/direct/test-direct.sh`, which has only `pal` and `libos`.)

Notes:

- The images are `saguaro-vm-aarch64` and `saguaro-vm-x86_64` (`container/Containerfile.vm-*`, tags below);
  set `CONTAINER_NAME` / `IMAGE_NAME` for other names. A checkout in a folder named
  `saguaro-<name>` gets its own container and guest CID range (see the header of `container/run.sh`); a folder
  named `gramine-<name>` (the earlier convention) is accepted too.
- The container runs with `--cap-add ALL` (virtiofsd serves the container's `/` as the guest's `/`)
  and `seccomp=unconfined` (the direct PAL calls `personality()`, which the default profile blocks).
- Images are tagged per Containerfile version (`saguaro-vm-x86_64:cf-<first 12 digits of the Containerfile's sha256>`): checkouts whose Containerfiles differ (branches) get their own images and
  never rebuild or retag each other's, and an existing image built from the same Containerfile is reused
  by tagging it, without a build. A checkout's container is recreated only when its own Containerfile
  changes; that discards what the container keeps outside the repository (its `/tmp`, an install
  prefix under `~/.local`). `IMAGE_NAME` selects the older behaviour (one tag, rebuilt when the
  Containerfile differs from its label). Images of old versions stay until removed (`podman image prune` or `podman rmi`).

## RISC-V (cross-build and test guest)

The RISC-V (RV64) port covers the direct PAL and is experimental. It is cross-built on an x86_64
machine (`container/run.sh --target riscv64` refuses other hosts) and tested in a RISC-V Linux guest under `qemu-system-riscv64` (user-mode emulation
cannot run it, because it has no seccomp). Everything runs unprivileged, in a container of its own:

    container/run.sh --target riscv64                       # shell in the cross container
    scripts/install.sh --target riscv64 --prefix "$PWD/build-riscv64/prefix"   # build and install
    scripts/riscv64/guest.sh start                          # boot the guest (first boot installs packages)
    scripts/riscv64/render-test-manifests.sh pal            # render the suite's manifests (also: libos)
    scripts/riscv64/run-suite.sh pal                        # PAL regression suite in the guest
    scripts/riscv64/render-test-manifests.sh libos
    scripts/riscv64/run-suite.sh libos                      # LibOS regression suite in the guest
    scripts/riscv64/guest.sh ssh 'apt-get install -y build-essential'   # once: a compiler in the guest
    scripts/riscv64/run-examples.sh helloworld bash redis   # example suites, built in the guest
    scripts/riscv64/guest.sh stop

The guest sees the repository at the same path as the container, so it runs the cross-built binaries
directly; this is why the prefix above lies inside the repository. The suites run from the guest's own
disk. The guest image has no compiler; examples other than `helloworld` need more packages (the list
installed by `.github/workflows/direct-riscv64-emulated.yml`). Design, status and limits: `dev docs: riscv/PLAN.md`.

## Building by hand

`scripts/install.sh` wraps `meson`. For the direct PAL:

    meson setup build -Dbuildtype=release -Ddirect=enabled -Dsgx=disabled -Dvm=disabled -Dtdx=disabled \
        -Dtests=enabled -Dlibc=glibc --prefix="$HOME/.local/saguaro"
    ninja -C build install

The aarch64 vm PAL recipe is in `dev docs: vm.md`. All options are in `meson_options.txt`. Called directly, meson defaults to `-Dlibc=glibc`; the
defaults in [Which libc](#which-libc) are set by `scripts/install.sh`.

## Troubleshooting

- `PAL failed setting personality failed: Function not implemented`: the direct PAL runs under a
  seccomp profile that blocks `personality()` (typically a default container). Run it natively or with
  `--security-opt seccomp=unconfined` (`container/run.sh` does this).
- `saguaro-vm` does nothing useful, or `/dev/kvm` is missing: the vm PAL needs hardware
  virtualisation on a machine of the *same* architecture; on AArch64 the launcher falls back to software emulation (TCG)
  without `/dev/kvm`, on x86_64 KVM is required.
- `[ VM exited with code 134 ]`: the guest kernel panicked; the lines above it hold the register dump.
- aarch64 `Mounting "file:<app>" ... failed: ENOENT`: the application binary is not built or is not a
  PIE; rebuild it with `-fPIE -static-pie` (the example's `Makefile` builds it that way).
- aarch64 `vhost-vsock: failed to open vhost device: Permission denied`: your user cannot open
  `/dev/vhost-vsock`; give it access (an ACL like `/dev/kvm`'s) and check that `vhost_vsock` is loaded.
