The mini-kernel shared by the `vm` and `tdx` PALs.

- The files in this directory are the **x86-64** implementation and rely on the `q35` (PCI-based)
  machine type in QEMU.
- `arch/aarch64/` holds the aarch64 counterparts (exception vectors, GICv3, PSCI SMP bring-up, device-tree
  parsing, VMSAv8-64 paging, scheduler/context switch) for QEMU's `virt` machine; only the `vm` PAL uses
  them (there is no aarch64 TDX). Files common to both architectures (virtio drivers, `pal_common_*`)
  stay in this directory. See `Documentation/devel/vm.md`.
