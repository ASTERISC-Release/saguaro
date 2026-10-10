# The `vm` PAL (unikernel) — build and run

The `vm` PAL (`pal/src/host/vm/` + `pal/src/host/vm-common/`) is a bare-metal unikernel:
a ring-0 Gramine kernel (own scheduler, page tables, GIC/PL011/fw_cfg/ECAM-PCI/virtio
drivers) that boots directly in QEMU and runs the LibOS + application at ring-3 — no
host OS underneath. On x86 it is the engine behind the `gramine-vm` launcher used by
the game examples; the aarch64 port is in progress (see `Documentation/devel/aarch64/vm-pal-plan.md` for the
design history and `Documentation/devel/aarch64/direct-pal-port-log.md` for the LibOS/PAL arch-layer groundwork).

## x86-64 (status: works, used by all game examples)

Inside the project podman container (`docker/build.sh` + `docker/exec.sh`), the stock
install builds it (`gramine-install.sh`):

```sh
cd <repo>/gramine-tdx
meson setup build-release/ --buildtype=release -Dtests=enabled \
    -Dskeleton=enabled -Ddirect=enabled -Dsgx=disabled -Dvm=enabled -Dtdx=disabled
ninja -C build-release/ && ninja -C build-release/ install
```

Run any example as usual (the launcher starts QEMU/q35 with the virtio-serial console,
vhost-user-fs via `virtiofsd`, vsock, fw_cfg args/envs/unixtime, and the fork's
`-gramine-args` extensions):

```sh
cd CI-Examples/graphics/<example> && make && gramine-vm <example>
```

## aarch64 (status: runs LibOS applications under KVM at SMP=1..4; see `Documentation/devel/aarch64/vm-pal-plan.md`)

Build (native aarch64, or cross; needs meson ≥ 1.x + gcc):

```sh
cd <repo>/gramine-tdx
meson setup build-vm-aarch64 -Dvm=enabled -Ddirect=disabled -Dsgx=disabled \
    -Dtdx=disabled -Dgraphics=disabled -Dtests=disabled --prefix=/tmp/vm-prefix
ninja -C build-vm-aarch64          # kernel: build-vm-aarch64/pal/src/host/vm/pal
```

`qemu-system-aarch64`, `dtc`, `virtiofsd` (Ubuntu ships it at `/usr/lib/qemu/virtiofsd`) and
`/dev/kvm` must be available -- in practice that is the `gramine-vm-aarch64` container
(`docker/build-vm-aarch64.sh`, `docker/exec-vm-aarch64.sh`), which also has the python
modules `gramine-manifest` needs.

**QEMU requirement (aarch64).** Use a **stock distribution QEMU** (`qemu-system-aarch64` from Ubuntu
24.04's `qemu-system-arm`, 8.2.x, is what every aarch64 result in this repository was measured with;
newer stock releases are expected to behave the same but are unverified). The QEMU fork used for the
x86 `vm`/`tdx` and graphics work does **not** build for aarch64 (it contains x86-only code, e.g.
`egl-redirect/xcb.c` inline asm and `util/sg-listener.c` with `KVM_VCPU_TSC_CTRL`) and is not needed:
the aarch64 `vm` PAL uses only upstream machine features (`virt` with `gic-version=3`, KVM,
`vhost-user-fs`, fw_cfg, PSCI). Select another binary with `QEMU_PATH=...`. Build the aarch64 `vm` PAL with `-Dgraphics=disabled`:
the graphics host redirection needs that fork's `sg-listener` agent, and without one the redirected
fd paths misbehave (`scripts/install.sh --vm` sets it for you).

### `gramine-vm`

On aarch64 meson installs `tools/gramine-vm-aarch64.in` as `gramine-vm` (the x86
`gramine-vm.in` cannot work here). Install the VM build and use it exactly like on x86:

```sh
ninja -C build-vm-container install          # prefix from `meson setup` (e.g. /tmp/vm-prefix)
export PATH=<prefix>/bin:$PATH PYTHONPATH=<prefix>/lib/python3.12/site-packages
gramine-manifest app.manifest.template app.manifest   # loader.entrypoint = <prefix>/.../libsysdb.so
gramine-vm app [args...]                              # runs app.manifest; exits with the guest's code
```

Environment knobs: `GRAMINE_CPU_NUM` (default 1), `GRAMINE_RAM_SIZE` (default 2G; a
manifest's `sgx/tdx.enclave_size` wins, floor 1G), `GRAMINE_ACCEL`/`GRAMINE_CPU`
(`kvm`/`host`), `QEMU_PATH`, `GRAMINE_VSOCK=1`, `GRAMINE_VM_CONSOLE_LOG=<file>`, `GDB=1`
(`-gdb tcp::9000 -S`). The guest powers itself off with PSCI `SYSTEM_OFF` (QEMU always
exits 0), so the launcher recovers the application's exit status from the
`[ VM exited with code N ]` console line (134 for a kernel panic).

Two things differ from a Linux/direct run and matter when preparing test binaries:
the guest's DRAM starts at `0x40000000` and the kernel maps memory 1:1, so
**applications must be PIE (ET_DYN)** -- a non-PIE binary linked at `0x400000` cannot be
mapped (`Failed to map file ... EINVAL`); `tools/vm-aarch64-stage/hello` is built
`-static-pie` for this reason.

### Running the libos regression suite in the VM

The existing pytest suite runs unchanged against the VM PAL; `GRAMINE_VM=1` makes the
harness (`python/graminelibos/regression.py`, `gramine-test run`) launch `gramine-vm`
instead of `gramine-direct`:

```sh
meson setup build-vm-container -Dvm=enabled -Ddirect=disabled -Dsgx=disabled -Dtdx=disabled \
    -Dtests=enabled -Dtests_pie=true --prefix=/tmp/vm-prefix     # tests_pie: see above
ninja -C build-vm-container install
export PATH=/tmp/vm-prefix/bin:$PATH PYTHONPATH=/tmp/vm-prefix/lib/python3.12/site-packages
export GRAMINE_VM=1 GRAMINE_CPU_NUM=2
cd libos/test/regression
gramine-test run helloworld                    # one test, manifest rebuilt on the fly
gramine-test pytest -- -q -rfEs test_libos.py  # the whole suite (~2 min)
```

(`-Dtests=enabled` skips the PAL regression tests unless direct/SGX is also enabled: they link
against `libpal.so`, which the bare-metal VM PAL does not provide.) The guest has one virtio
console, so the harness gets stdout+stderr+Gramine's log as one stream and uses it for both.
With `GRAMINE_VM=1`, `libos/test/regression/conftest.py` applies `vm_skip.toml`: tests that cannot run
on the VM PAL are skipped, and failures whose cause is not yet understood are marked xfail, each with a
`[VM:<category>]` reason, so a plain `gramine-test pytest` is a usable pass/fail gate. Baseline
(2026-10-10, SMP=2, KVM): **79 passed, 95 skipped (34 suite-guard + 61 VM), 3 xfailed, 0 failed** in about a
minute (raw run without the list, `GRAMINE_VM_SKIPLIST=0`: 79 passed / 64 failed / 34 skipped). `-rs` lists the skip reasons; a skip that no longer applies (the platform grew the feature) is
removed from `vm_skip.toml`, and an xfail that starts passing shows up as XPASS. `GRAMINE_TEST_TIMEOUT`
overrides the per-run timeout (default 40 s under the VM, 10 s otherwise).

### Development runner

`tools/run-vm-aarch64-dev.sh` is the original bring-up script (hello only, console to a
file, `RUN_TIMEOUT` backstop); `gramine-vm` supersedes it for everything but kernel
debugging. The non-obvious parts both encode (all requirements, not choices):

- **DTB**: QEMU gives ELF `-kernel` images *no* DTB, so the script dumps the machine
  DTB (`-machine dumpdtb=...`), repacks it with `dtc` and loads it as a RAM blob at
  `0x40000000` via `-device loader`; the kernel authenticates it by FDT magic.
- **`-machine virt,gic-version=3,highmem-ecam=off`**: this fork defaults
  `highmem_ecam=true`, which moves ECAM to the 256 GB window (and drops the DTB PCI
  node); the kernel's boot identity map covers `0x3f000000` only.
- **`-fw_cfg name=opt/gramine/unixtime_s,string=$(date +%s)`**: `time_init()` rejects
  wall-clock outside `[2023, 2033)`.
- **`-object memory-backend-memfd,id=ram1,size=2G,share=on -machine
   memory-backend=ram1`** + a running `virtiofsd` +
  `-device vhost-user-fs-pci,...,tag=graminefs`: the vm kernel requires the virtio-fs
  device at boot.
- **`iommu_platform=off` on the virtio devices**: virtio feature validation fails
  otherwise on machines without an enumerated IOMMU.
- **`-m 2G` minimum**: the kernel carries a 64 MB static shared-memory pool in `.bss`
  and keeps its allocator floor above the image (`__text_end`).

Boot and bring-up history lives in `Documentation/devel/aarch64/vm-pal-plan.md`. `VM exited with code 1` with a
`error: PAL failed ...` line means an early boot `INIT_FAIL` -- the message names the failing step.
