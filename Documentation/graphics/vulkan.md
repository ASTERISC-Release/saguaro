# Vulkan under `gramine-direct` and `gramine-vm`

> All numbers on this page are experimental measurements from one machine, taken during development; they are not benchmarks.

**What it does.** Vulkan applications (Mesa's `anv` on Intel, `radv` on AMD) run under both
routes. Under `gramine-direct` they need nothing beyond the direct route's keys. Under
`gramine-vm` the device path goes through the forwarded DRM ioctls and presentation goes through
a layer, because Mesa's X11 window-system integration cannot pass dma-buf fds over the vsock relay.

**How it works.** *Device path:* the ioctl forwarding was made good enough for a Vulkan driver:
errno values propagate, absolute deadlines of syncobj waits are translated to the host's clock
(they were 57 years in the future before), fds inside syncobj and dma-buf requests are translated,
the communication page has four request slots so that several threads submit at once, and arena
free lists let `munmap` return GPU buffers without aborting QEMU. On AMD, write-combined memory
was incoherent until KVM's "ignore guest PAT" quirk was turned off for the guest. *Presentation:*
an implicit layer in `libgraphics.so` (`VK_LAYER_GRAMINE_present`, on with
`EGL_VULKAN_PRESENT=1`) answers the surface and swapchain calls itself: the application renders
into exportable images, the layer blits them and hands the dma-bufs to the host listener, which
shows them in its window; FIFO mode paces with a yielding wait (the VM PAL's timed sleeps have a
100 ms tick, which made FIFO run at 10 FPS). The layer hides six presentation extensions so that
DXVK and VKD3D-Proton do not pick paths it does not serve; Mesa's device-select layer must be off
(`NODEVICE_SELECT=1`), as its DRI3 query hangs over the relay. *Checks:* `vk-headless` (14 steps:
instance, device, memory, compute, fences, timeline semaphores, submit latency, render, map churn,
exports), `vk-two` (several processes in one session), `gfx-fdpass` (a host fd over `SCM_RIGHTS`
to a forked and to an exec'd process), run together by `test/vulkan/vm-check.sh` from demand-paged
processes (`VKH_DEVICE=Intel` selects the Intel device on a two-GPU machine).

**Key results** (2026-10-15, 1920x1080, native / direct / VM). Quake II with the Vulkan
renderer: Intel 371 / 374 / 318 FPS (VM 86 % of native), AMD 3213 / 3000 / 2315 FPS (72 %).
`vkcube`: Intel 356 / – / 349 FPS (98 %), AMD 27,473 / – / 10,989 FPS (40 %). `vkmark` score:
Intel 630 / – / 493 (78 %), AMD 35,553 / – / 18,459 (52 %). Submit-and-wait latency of an
empty command buffer: Intel 39 µs natively, 43 to 47 µs in the VM; AMD 34 against 35 to 37 µs. On
2026-10-20 the headless suite passed on both GPUs from demand-paged processes, on Intel through the
i915 entries of the pointer table ([../GRAPHICS.md](../GRAPHICS.md)).

**Findings worth knowing.** Timeline-semaphore waits returned `VK_ERROR_UNKNOWN` instead of
`VK_TIMEOUT` while errno was lost on the forwarded path. An idle window at 60 Hz kept QEMU at about
205 % CPU because the listener spins; an opt-in backoff lowers it at the cost of OpenGL frame rate.

**Limits and open items.** Two-thread submission is slower in the VM than natively. The
listener's idle CPU use. The picture of the headless and rate runs was not always confirmed on
screen. The pointer table is built from the requests Mesa 25.2 issues; driver requests outside it
are refused ([../GRAPHICS.md](../GRAPHICS.md)).
