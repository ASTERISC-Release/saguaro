# The `vm` PAL (unikernel) — build and run

The `vm` PAL (`pal/src/host/vm/` + `pal/src/host/vm-common/`) is a bare-metal unikernel:
a ring-0 Gramine kernel (own scheduler, page tables, GIC/PL011/fw_cfg/ECAM-PCI/virtio
drivers) that boots directly in QEMU and runs the LibOS + application at ring-3 — no
host OS underneath. On x86 it is the engine behind the `gramine-vm` launcher used by
the game examples; the aarch64 port is in progress (see `ARM64_MILESTONE_B.md` for the
design history and `ARM64_MILESTONE_A.md` for the LibOS/PAL arch-layer groundwork).

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

## aarch64 (status: boots the kernel; app handoff pending)

Build (native aarch64, or cross; needs meson ≥ 1.x + gcc):

```sh
cd <repo>/gramine-tdx
meson setup build-vm-aarch64 -Dvm=enabled -Ddirect=disabled -Dsgx=disabled \
    -Dtdx=disabled -Dtests=disabled --prefix=/tmp/vm-prefix
ninja -C build-vm-aarch64          # kernel: build-vm-aarch64/pal/src/host/vm/pal
```

`qemu-system-aarch64` (≥ 9.x, this repo's fork also builds with `build-arm/`),
`dtc` and `virtiofsd` (Ubuntu ships it at `/usr/lib/qemu/virtiofsd`) must be available.
Then run it with:

```sh
tools/run-vm-aarch64-dev.sh        # add "-accel kvm" as an arg to use KVM
```

Unlike x86 there is no `gramine-vm`-style launcher yet; the script is the current
bring-up recipe. The non-obvious parts it encodes (all requirements, not choices):

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

Boot progress log lives in `ARM64_MILESTONE_B.md`; the kernel currently runs through
memory/slab init, timers, PCI enumeration and virtio-console TX, and stops at the
virtio-fs device handshake. `VM exited with code 1` with a
`error: PAL failed ...` line means an early boot `INIT_FAIL` — the message names the
failing step.
