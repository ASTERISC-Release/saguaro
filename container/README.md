# Containers for the `vm` PAL and the test batteries

One image per architecture, both Ubuntu 24.04 with stock QEMU, virtiofsd and the build/test toolchain.
Each needs a **native** host of that architecture (no emulation) with working KVM (`/dev/kvm`) for
`gramine-vm`; the direct PAL needs no devices. The repository is mounted at `/root/gramine-multiarch`.

| | aarch64 | x86_64 |
|---|---|---|
| image / container | `gramine-vm-aarch64` | `gramine-vm-x86_64` |
| build + start | `container/build-vm-aarch64.sh` | `container/build-vm-x86_64.sh` |
| shell / one-shot / stop | `container/exec-vm-aarch64.sh [cmd|down]` | `container/exec-vm-x86_64.sh [cmd|down]` |
| recipe | `Containerfile.vm-aarch64` | `Containerfile.vm-x86_64` |

```sh
container/build-vm-aarch64.sh                 # (re)create the container
container/exec-vm-aarch64.sh 'cd /root/gramine-multiarch && scripts/install.sh --vm'
container/exec-vm-aarch64.sh 'cd /root/gramine-multiarch && test/test-vm.sh'
```

The containers run with `--cap-add ALL` (virtiofsd serves the container's `/` as the guest's `/`);
the x86_64 one also uses `seccomp=unconfined`, because the direct PAL calls `personality()` (the default
profile blocks it; the aarch64 container needs the same option for direct-PAL tests).

QEMU: stock distribution QEMU only (see `Documentation/devel/vm.md`). The TDX and graphics paths need the
repository's QEMU fork, which is not in these images: build it separately and set `QEMU_PATH`.

The x86_64 recipe has been written but **not built or run** (the development host is aarch64); treat it as
untested until it has been exercised on an x86_64 machine.
