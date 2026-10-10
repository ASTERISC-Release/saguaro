# Container for the aarch64 `vm` PAL

Native **aarch64 host with working KVM** required (TCG is out of scope). The image is Ubuntu 24.04
with stock QEMU 8.2.x, virtiofsd, dtc, gdb and the meson/gcc toolchain; see `README-vm.md` for the
QEMU requirement and the build/run recipe.

```sh
docker/build-vm-aarch64.sh                 # build the image, (re)create container gramine-vm-aarch64
docker/exec-vm-aarch64.sh                  # shell in the container (repo mounted at /root/gramine-multiarch)
docker/exec-vm-aarch64.sh '<command>'      # one-shot command
docker/exec-vm-aarch64.sh down             # stop and remove the container
```

The container needs `--cap-add ALL` (virtiofsd serves the container's `/` as the guest's `/`) and
`--device /dev/kvm`; both are set by `build-vm-aarch64.sh`.
