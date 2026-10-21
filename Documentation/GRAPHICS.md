# Graphics: the two routes, the pointer table, Proton games

> **Disclaimer.** All numbers in this document are experimental measurements from a single machine, taken during development. They are not benchmarks and they will change.

## The two routes

**Direct** (`gramine-direct`, x86_64 and AArch64): the application runs as a host process under
the LibOS and opens the host's render nodes and X server itself; stock Mesa or NVIDIA's driver
does the work, and the LibOS translates only the file descriptors and the absolute deadlines
inside the device ioctls. Fast, not isolated. Details: [graphics/direct.md](graphics/direct.md),
[graphics/aarch64.md](graphics/aarch64.md).

**Redirected** (`gramine-vm`, x86_64): the application runs in a microVM without a GPU driver. A
preload library, `libgraphics.so`, forwards its EGL, GBM and DRM calls through a shared
communication page to a listener in a QEMU fork, which uses the host GPU and presents the frames;
the X connection is relayed over vsock. Vulkan presents through a layer in the same library. With
`sys.experimental__multiprocess` off, the single guest process is identity-mapped and the
listener reads the application's buffers at the addresses it gets. With the key on (the
default), every process has private page tables, and the LibOS copies each forwarded ioctl
argument, and what its pointers point to, into physically contiguous bounce buffers, rewriting
the pointers to physical addresses: which fields are pointers comes from a per-request table in
`libos/src/sys/libos_ioctl.c`. Details: [graphics/vm.md](graphics/vm.md),
[graphics/vulkan.md](graphics/vulkan.md).

## Known issue and its fix

*Cause.* With the multiprocess key on, the first process is demand-paged by design (the VM PAL
creates it with private page tables), so every graphics manifest without the key took the bounce
path once the key's default became `true`; the pointer table covered the AMD drivers' requests
and no i915 request, so on an Intel GPU Mesa's first i915 request (`I915_GETPARAM`) failed with
`EFAULT` and the application opened no window ("Could not get intel_device_info"). On an AMD GPU
`AMDGPU_GEM_OP` failed the same way, which the applications survived.

*Fix.* The table and its code now translate the i915 requests of Mesa 25.2 (`GETPARAM`, `QUERY`
and its items, the extension chains of `GEM_CREATE_EXT` and `GEM_CONTEXT_CREATE_EXT`, the
context parameters, `GEM_EXECBUFFER2` with objects, relocations and fences) and `AMDGPU_GEM_OP`;
user-pointer objects are refused. With the default key, on an Intel iGPU and an AMD
GPU: `glxgears` (430 FPS Intel), Quake II (OpenGL 410 / Vulkan 331 FPS Intel; 3000 / 2277
AMD), Minetest and SuperTuxKart windows on both GPUs, `test/vulkan/vm-check.sh` on both Vulkan
devices (`VKH_DEVICE=Intel` for anv), the VM LibOS regression unchanged (168 passed).

## The guard

DOOM on the AMD path then crashed in 4 of 12 runs: a 260-byte `memmove` in Wine's Unix side
reads past the end of a 1 MiB mapping, and the next page was a LibOS thread stack, because the
LibOS's own mappings and the application's were placed by the same top-down allocator and
interleaved; the LibOS ended the process with an internal fault. Now the VM PAL names a range (the
top 4 GiB of the 512 GiB address space) for the LibOS's own mappings, the application's stay below
it, contiguous as under Linux, and a user-mode access to a LibOS mapping is the application's
`SIGSEGV` instead of the end of the process. The regression test `vma_neighbor` (LibOS suite)
interleaves mappings with thread creation and reads past every mapping: the earlier LibOS dies
there, the current one survives. The soak after the guard is in the table's footnote.

The guard's changed timing then exposed a second, older fault of the AMD EGL path (`glxgears`,
Minetest: `Internal memory fault at 0x00000010` after the third GBM buffer): `lseek` and `fstat`
on a forwarded host object whose fd the graphics library had registered under its raw host
number took the ordinary file path, which such a handle does not have; they now forward these
handles to the host. Verified: Minetest and `glxgears` on the AMD GPU, three runs each.

## Proton games

GE-Proton 11's Wine with DXVK in `gramine-vm` on the redirected route (`test/proton-vm/`),
on both GPUs; the guest with 4 vCPUs and 12 GiB; "native" is the same game through the
same Wine prefix on the host, vsync off where stated; every window went full screen at 3840x2160;
the games were driven by a short key script; the FPS are the DXVK overlay at the stated point and
the ratio is against native on the same GPU. Details: [graphics/proton.md](graphics/proton.md).

| Game | Bitness | API | GPU | Native FPS | `gramine-vm` FPS (ratio) | Result | First blocker |
|---|---|---|---|---|---|---|---|
| DOOM I Enhanced | x86-64 | D3D11 | AMD | 2294 to 2347, E1M1 | 72 (0.03), E1M1 | plays the first level | present path bound; the crash before the guard¹ |
| DOOM I Enhanced | x86-64 | D3D11 | Intel | 68 to 69, E1M1 | 59.5 (0.86), E1M1 | plays the first level | – |
| Psychonauts | i386 | D3D9 | AMD / Intel | 29.8 / 29.7, intro | 29.1 / 29.2 (1.0), intro | starts, intro plays | needs a null ALSA device |
| The Witcher: Enhanced Edition | i386 | D3D9 | AMD / Intel | 299.9 / 85.7, intro | 301.5 (1.0) / 50.4 (0.59), title | starts, title screen | – |
| BioShock 2 Remastered | i386 | D3D9/D3D10 | AMD / Intel | 144 (vsync) / 96.6, menu | 64.6 (0.45) / 58.7 (0.61), menu | main menu | – |
| Tomb Raider: Underworld | i386 | D3D9 | both | 60 (game cap), start-up dialogs | – | does not start | `status c0000018`: a non-relocatable executable's low range is not free in the guest |

¹ Before the guard 4 of 12 runs crashed before the first present (the LibOS without the
pointer-table commit: 0 of 6). After the guard: 12 of 12 runs clean (100 s each, 66 to 70 presents/s), and DOOM on the Intel GPU still reaches E1M1 at 59.4 FPS.

The AMD guest figures are bound by the present path (60 to 70 presents/s in FIFO mode), not by
the GPU; the Intel figures are close to native. Gameplay beyond the first level or menu, sound
and input were not checked.
