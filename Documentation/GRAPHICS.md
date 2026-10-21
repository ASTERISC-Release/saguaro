# Graphics: architecture, the pointer table of the redirected route, Proton games

This page summarises how applications reach a GPU from this tree, records one issue of the
redirected route with its root cause and fix, and lists the results of Windows games under
Proton. The guides are `quickstart/graphics-direct.md` and `quickstart/graphics-vm.md`; the
examples are in `CI-Examples/graphics/`.

## Architecture

**The direct route** (`gramine-direct`, x86_64 and AArch64). The application runs as a host
process under the LibOS and opens the host's render nodes itself; stock Mesa talks to the kernel
driver through the device ioctls, which the LibOS passes through (`libos/src/fs/hostfd/`), and
presents through DRI3 on the host X server. The LibOS translates only what differs between the
application's view and the host's: file descriptors inside ioctl arguments and absolute
deadlines (`libos/include/libos_device_ioctl.h`).

**The redirected route** (`gramine-vm`, x86_64). The application runs in a microVM. A preload
library in the guest, `libgraphics.so` (`libos/src/graphics/`), takes the EGL, GBM and DRM calls
and forwards them through a shared comm page to a listener in a QEMU fork
([Tirith](https://arxiv.org/abs/2609.17525)), which opens the host GPU with stock Mesa. The
application's own X connection is relayed over vsock; the listener presents the frames on the host
display. Mesa inside the guest issues its DRM ioctls on a forwarded host file descriptor; the LibOS
hands each of them to the listener (`forwarded_ioctl()` in `libos/src/sys/libos_ioctl.c`), which
issues it on the host.

**Identity-mapped versus demand-paged memory.** The listener reads the ioctl argument, and the
arrays the argument points to, at guest-physical addresses. With `sys.experimental__multiprocess`
off, the guest has one process and its memory is identity-mapped (a user virtual address is the
guest-physical address), so the LibOS passes the application's pointers through unchanged. With the
key on (the default), every process has its own page tables, the first one too, and the LibOS takes
the bounce path (`libos/src/sys/libos_graphics_bounce.c`): the argument of each forwarded ioctl is
copied into a physically contiguous bounce buffer, every pointer inside it is rewritten to the
physical address of a bounced copy of what it points to, the outputs are copied back after the
call, and the application's pointer values are restored. Which fields are pointers, and how large
the pointed-to data is, comes from a per-request table and a few request-specific routines in
`libos_ioctl.c`, built from the kernel's uapi headers.

## Known issue (2026-10-20): Intel GPUs from a demand-paged process

**Symptom.** With the default manifest (the multiprocess key not set) a graphics example on an
Intel GPU printed `MESA: error: Kernel 4.15 required to read the CS timestamp frequency` and
`MESA: warning: Could not get intel_device_info`, opened no window and waited in a futex. On an
AMD GPU the same manifest ran. Setting `sys.experimental__multiprocess = false` in the manifest
avoided the failure.

**Root cause.** Two layers:

1. *Policy.* The VM PAL gives the first process private page tables whenever the multiprocess key
   is on, with graphics configured as well (`pal/src/host/vm/pal_main.c`, the initial address
   space is created with `identity_memory = false`). This is by design: the Windows-games work
   needs a demand-paged first process with a large address range, and the redirected route was
   changed to translate addresses in the LibOS instead of relying on an identity mapping. When the
   key's default became `true`, every graphics manifest without the key moved from the
   identity-mapped path to the bounce path.
2. *The table.* The pointer table of the bounce path covered the requests of the AMD drivers
   (`radeonsi`, `radv`: `AMDGPU_CS` and its chunks, `AMDGPU_INFO`, the `SYNCOBJ` arrays,
   `DRM_VERSION`) and no i915 request. Every pointer inside an i915 argument reached the host
   kernel as a guest virtual address and failed with `EFAULT`; the first one Mesa's `iris` follows
   is `DRM_IOCTL_I915_GETPARAM`'s `value`, which is the device probe. The trace of the same
   example on the AMD GPU showed one gap there too: `DRM_IOCTL_AMDGPU_GEM_OP` (the buffer query of
   every imported GBM buffer) returned `EFAULT` on every call, which the program survived.

**Fix.** `libos/src/sys/libos_ioctl.c` translates the i915 requests that Mesa 25.2 issues
(`GETPARAM`; `QUERY` and its items; the extension chains of `GEM_CREATE_EXT` and
`GEM_CONTEXT_CREATE_EXT`; the context parameter value of `GEM_CONTEXT_SETPARAM` and `GETPARAM`;
`GEM_EXECBUFFER2` with its objects, relocations, fence array or timeline-fence extension) and
`AMDGPU_GEM_OP`'s output, and refuses the user-pointer objects (`GEM_USERPTR`), whose memory
cannot be bounced. The fixup records keep the slot each pointer was rewritten in, so the
application's values are restored in every bounced copy before anything is copied back (the
query items are both an output and hold pointers). The layouts come from the kernel's uapi
headers (the i915 and amdgpu DRM headers); the set of requests from LibOS traces of the examples.

**Evidence** (2026-10-20, the container of `quickstart/graphics-vm.md`, Mesa 25.2.8, the
manifests' default for the multiprocess key, an Intel UHD 770 and an AMD Radeon RX 9070):

| Check | Intel | AMD |
|---|---|---|
| `glxgears` before the fix | no window; `I915_GETPARAM` = `EFAULT` in the trace | runs; 9 of 9 `AMDGPU_GEM_OP` = `EFAULT` |
| `glxgears` after the fix | window, 430 FPS; the trace has the same requests and return values as the identity-mapped run | no ioctl error in the trace |
| Quake II timedemo, OpenGL / Vulkan (`test/vulkan/q2-timedemo.sh vm`) | 410 / 331 FPS | 3000¹ / 2277 FPS |
| Minetest, one minute | window, 60.4 FPS | window, 60.6 FPS |
| SuperTuxKart, 20 s race (`run-startup`) | window, 93.1 FPS | window, 1272.6 FPS |
| `test/vulkan/vm-check.sh` (Vulkan from demand-paged processes) | pass (`VKH_DEVICE=Intel`, anv) | pass (radv) |
| VM LibOS regression (`test/vm/test-vm.sh libos`) | 168 passed, 60 skipped, 2 xfailed, 2 xpassed (unchanged) | |
| Builds | direct PAL (x86_64) and the riscv64 cross-build compile the change; AArch64 not built (no toolchain at hand) | |

¹ One of two runs gave no result within 120 s; the second gave 3000 FPS.

The regression check of the bounce path is `test/vulkan/vm-check.sh`: it runs the headless Vulkan
suite from one and from two demand-paged processes and the fd-passing check. The guest's Vulkan
loader lists every GPU of the host; `VKH_DEVICE=Intel test/vulkan/vm-check.sh` selects the Intel
device (anv, the i915 entries), the default is device 0. Run one `gramine-vm` at a time: two
instances share the relay port and the second fails to start.

## Proton games

Results of the Windows games under GE-Proton's Wine in `gramine-vm` on the redirected route
(`test/proton-vm/`), each on both GPUs. "Native" is the same game through the same Wine prefix on
the host; the FPS ratio is against native on the same GPU.

| Game | Bitness | API | GPU | Native FPS | `gramine-vm` FPS | Result | First blocker |
|---|---|---|---|---|---|---|---|
| (pending) | | | | | | | |
