# The host GPU under `gramine-direct`

> All numbers on this page are experimental measurements from one machine, taken during development; they are not benchmarks.

**What it does.** An OpenGL or Vulkan application under `gramine-direct` uses the host's GPU
directly, with the host's Mesa (or NVIDIA's driver) and the host's X server. It is the fast route
and the reference for the VM route; it is not a sandbox.

**How it works.** Three opt-in manifest keys open the way: raw device ioctls on the render nodes
(`sys.insecure__allow_raw_device_ioctl`), host Unix sockets with fd passing for the X connection
(`sys.insecure__host_unix_sockets`; DRI3 presentation passes dma-buf fds over it, which rules out
a TCP relay), and host symlinks for libdrm's sysfs queries. The LibOS translates what differs
between the application's view and the host's: file descriptors inside DRM, dma-buf and sync-file
ioctl arguments (prime handles, syncobjs, sync files) become LibOS handles, and absolute
`CLOCK_MONOTONIC` deadlines inside syncobj and fence waits are moved to the host's clock, since
Gramine's clock is real time. Stock Mesa with DRI3 is the default; the `libgraphics.so` preload
library of the VM route also works here (`PRELOAD=1`) and presents through its own X connection.
NVIDIA's proprietary driver works through the same keys with its own device nodes.

**Key results** (1920x1080). Intel iGPU: Quake II 452 FPS against 454 FPS
natively (99.8 %), `glxgears` 597 FPS (100 %), `vkcube` 99 % of native. AMD GPU: `glxgears`
about 34,000 FPS against about 45,000 natively (76 %: the per-frame system calls of a light
program show), SuperTuxKart and Minetest 89 to 91 %, `vkcube` 69 %. Quake II with the Vulkan
renderer: 374 FPS against 371 natively on Intel, 3000 against 3213 on AMD (93 %).

**Findings worth knowing.** A `poll()` that timed out did not zero `revents`, which broke the
NVIDIA driver's event handling (fixed in the LibOS for every PAL). The driver-specific fence fds
of i915 and amdgpu (`I915_EXEC_FENCE_IN`/`OUT`, `AMDGPU_FENCE_TO_HANDLE`) are not translated;
Mesa does not use them on these paths. The preload library is only built with a VM-type PAL in the
install.

**Limits and open items.** The keys give the application the whole GPU (command submission is
unchecked), so this route provides no isolation. Sound is not supported. A manifest names one
render node; several GPU nodes at once are untested, as are CUDA and compute. The TDX PAL does not
build in this tree. See [aarch64.md](aarch64.md) for the same route on AArch64 and
[vulkan.md](vulkan.md) for Vulkan.
