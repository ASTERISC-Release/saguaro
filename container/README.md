# Containers for the `vm` PAL and the test batteries

One script does everything: `container/run.sh` detects the machine's architecture (aarch64 or x86_64),
builds the matching image if it is missing or its Containerfile changed, creates/starts the long-lived
container, and attaches a shell. Both images are Ubuntu 24.04 with stock QEMU, virtiofsd and the
build/test toolchain; the repository is mounted at `/root/gramine-multiarch`.

```sh
container/run.sh                      # shell in the container for this machine
container/run.sh '<command>'          # one-shot command (no tty, pipes cleanly)
container/run.sh --rebuild            # force an image rebuild + container recreation
container/run.sh down                 # stop and remove the container (image is kept)

container/run.sh 'scripts/install.sh --vm'     # e.g. build the vm PAL inside
container/run.sh 'test/test-vm.sh'             # ... and run the vm battery
```

| | aarch64 | x86_64 |
|---|---|---|
| image / container | `gramine-vm-aarch64` | `gramine-vm-x86_64` |
| recipe | `Containerfile.vm-aarch64` | `Containerfile.vm-x86_64` |

`CONTAINER_NAME` / `IMAGE_NAME` override the names. A changed Containerfile triggers a rebuild, and a
container built from an older image is recreated (its `/tmp` is discarded; files in the repository
survive).

Each needs a **native** host of the same architecture (no emulation) with working KVM (`/dev/kvm`) for
`gramine-vm`; the direct PAL needs no devices. The container runs with `--cap-add ALL` (virtiofsd serves
the container's `/` as the guest's `/`) and `seccomp=unconfined` (the direct PAL calls `personality()`,
which the default profile blocks).

QEMU: stock distribution QEMU only (see `Documentation/devel/vm.md`). The TDX and graphics paths need the
repository's QEMU fork, which is not in these images: build it separately and set `QEMU_PATH`.
