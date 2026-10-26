# Containers for the `vm` PAL and the test batteries

One script does everything: `container/run.sh` detects the machine's architecture (aarch64 or x86_64),
builds the matching image if it is missing or its Containerfile changed, creates/starts the long-lived
container, and attaches a shell. Both images are Ubuntu 24.04 with stock QEMU, virtiofsd and the
build/test toolchain; the repository is mounted at `/root/<checkout folder>` (for example `/root/saguaro`).

```sh
container/run.sh                      # shell in the container for this machine
container/run.sh '<command>'          # one-shot command (no tty, pipes cleanly)
container/run.sh --rebuild            # force an image rebuild + container recreation
container/run.sh down                 # stop and remove the container (image is kept)

container/run.sh 'scripts/install.sh --vm'     # e.g. build the vm PAL inside
container/run.sh 'test/quick.sh vm'               # ... and run the regression tier of the vm PAL
```

| | aarch64 | x86_64 |
|---|---|---|
| image / container | `saguaro-vm-aarch64` | `saguaro-vm-x86_64` |
| recipe | `Containerfile.vm-aarch64` | `Containerfile.vm-x86_64` |

`container/run.sh --target riscv64 [...]` uses a third container instead, `saguaro-riscv64-cross`
(`Containerfile.riscv64-cross`, x86_64 hosts only): the riscv64 cross toolchain and
`qemu-system-riscv64` for the test guest of the RISC-V direct-PAL port (see `INSTALL.md`).

`CONTAINER_NAME` / `IMAGE_NAME` override the names. Images are tagged per Containerfile version
(`<name>:cf-<first 12 digits of the Containerfile's sha256>`), so checkouts with different Containerfiles
do not rebuild or retag each other's images, and an image built earlier from the same Containerfile is
reused without a build. A container is recreated only when its own checkout's Containerfile changes
(what it keeps outside the repository, such as `/tmp` and install prefixes, is discarded). With
`IMAGE_NAME` set, that single tag is rebuilt when the Containerfile differs from its label (the older
behaviour).

Each needs a **native** host of the same architecture (no emulation) with working KVM (`/dev/kvm`) for
`saguaro-vm`; the direct PAL needs no devices. The container runs with `--cap-add ALL` (virtiofsd serves
the container's `/` as the guest's `/`) and `seccomp=unconfined` (the direct PAL calls `personality()`,
which the default profile blocks).

QEMU: the images contain stock distribution QEMU only (see `dev docs: vm.md`). The graphics redirection
and the emulated SEV and TDX use the QEMU backends pinned as git submodules under `vmm/` of the checkout
(`vmm/graphics`, `vmm/sev-tcg`, `vmm/tdx-tcg`). They are part of the mounted checkout; fetch and build one
inside the container with `scripts/qemu.sh build <name>` (the image has the packages to build them), and
select it with `QEMU_PATH=$(scripts/qemu.sh path <name>)`. For graphics on x86_64, `run.sh` passes the DRM
render nodes, the host X socket and `DISPLAY` into the container. See `dev docs: graphics/running.md`.

AMD SEV (see `dev docs: sev/`): when the host has `/dev/sev`, `run.sh` passes it into the
container, with `--group-add keep-groups` when the user can open it through a group (for example
`sev`, mode 0660). The SEV-SNP and IGVM capable QEMU, firmware and tools are not in the image:
`container/sev/build-sev-tools.sh` builds them into a `sev-tools/` directory next to the checkout,
which `run.sh` mounts at `/root/sev-tools` when it exists (`SEV_TOOLS_DIR` names another one).
Select that QEMU with `QEMU_PATH=/root/sev-tools/prefix/bin/qemu-system-x86_64`.
`container/sev/smoke-test.sh none|sev|sev-es|snp` boots a stock Linux guest at each level.
