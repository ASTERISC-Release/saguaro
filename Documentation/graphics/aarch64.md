# Graphics on AArch64

> All numbers on this page are experimental measurements from one machine, taken during development; they are not benchmarks.

**What it does.** The direct route ([direct.md](direct.md)) on an AArch64 machine with an NVIDIA
GPU and NVIDIA's proprietary driver: OpenGL applications under `saguaro-direct` draw on the GPU
through the host's X server. The VM route is not available on AArch64.

**How it works.** The same manifest keys as on x86_64 pass the device nodes and the X socket
through. NVIDIA's driver embeds file descriptors in its own ioctls (the register-fd escape and
the resource-manager controls), which the LibOS translates like the DRM ones, and its GLX client
passes `socketpair` ends to the X server, which the host Unix-socket support had to accept. The
device mappings themselves needed no change. Two generic LibOS bugs surfaced: `poll()` not
zeroing `revents` on a timeout (the driver's event loop stalled) and `mremap` growing a mapping
in place by copying over live memory (LuaJIT in Minetest crashed); both fixed for every PAL.

**Key results** (NVIDIA driver 580, 1920x1080, native against `saguaro-direct`):
`glxgears` 5117 FPS against 5143 (99.5 %); Quake II 2830 FPS against 2839 (99.7 %); SuperTuxKart
99.1 %; Minetest 87.6 %, its overhead being frequent system calls (`getpid`, reads of
`/proc/meminfo`) that trap into the LibOS. Software rendering with `llvmpipe` runs at 93 to 100 %
of native.

**Findings worth knowing.** The QEMU fork of the VM route does not build for `aarch64-softmmu`:
its listener relies on x86 assembly, the KVM TSC and an x86 physical memory layout, so
`saguaro-vm` with graphics is marked "not available" rather than "planned" on AArch64. Input
was exercised with synthetic X events only (the session was locked during the measurements).

**Limits and open items.** No VM route. Vulkan and CUDA are untested on the board. The manifests
mount one NVIDIA device node; multi-GPU boards are untested. Real input devices and sound were not
tried.
