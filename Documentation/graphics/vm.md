# Redirected graphics for `gramine-vm`

> All numbers on this page are experimental measurements from one machine, taken during development; they are not benchmarks.

**What it does.** An unmodified OpenGL application (stock Mesa, EGL or GLX) runs inside the
`gramine-vm` microVM and draws on the host's GPU. The guest has no GPU driver and no
paravirtual device: the GPU work is forwarded to a listener on the host, which is the Tirith
design (see the paper cited in the quickstart) ported into this tree. x86_64 only.

**How it works.** A preload library in the guest, `libgraphics.so`, takes the application's EGL,
GBM and DRM calls. Requests go through a shared communication page at a fixed guest-physical
address to a listener thread in a QEMU fork (`qemu-tirith`), which opens the host's render node
with the host's Mesa, allocates the GPU buffers and presents the frames in a window of the host X
server. Buffers live in two guest-physical arenas (a data arena and a hugepage arena) that the
listener maps as GEM objects; Mesa's DRM ioctls on the forwarded render node are relayed by the
LibOS. The application's own X connection (window, events) is relayed over vsock, and key, button
and pointer events arrive through a shared ring in the communication page, which is faster than
the X path. The guest memory layout (arenas, communication page, RAM size) is computed by the
launcher at start and handed to the guest PAL and to QEMU, so the guest RAM is not fixed: the
floor is 4 GB (5 GB with 2 MB hugepages), of which about 2 GB are the arenas. With the
multiprocess key on (the default), the guest is demand-paged and the LibOS translates the
pointers inside the forwarded ioctls through bounce buffers ([../GRAPHICS.md](../GRAPHICS.md)).

**Key results** (Intel UHD 770 unless stated, Mesa 25.2, one vCPU, 2026-10). `glxgears`: about
495 FPS under the VM against about 504 FPS natively (98 %), the same at 4 GB and 8 GB of guest
RAM; 2 MB hugepages did not change the frame rate and made the GPU start about 190 ms faster.
SuperTuxKart: 63 to 79 FPS at 8 GB against 91.5 FPS natively (70 to 85 %); it does not fit in
4 GB. Quake II (OpenGL timedemo, 1920x1080): about 410 FPS on the Intel GPU and about 3000 FPS on
the AMD RX 9070 under the VM. Minetest: about 10 FPS against 62.8 FPS natively (16 %) with one
vCPU; 60 FPS with four vCPUs on both GPUs (2026-10-20, capped by its own setting). Boot of a
graphics guest: about 0.8 s at 8 GB after the launch-time layout and the dropped zero-fill,
against 2.9 s before. On 2026-10-20, with the default multiprocess key and the pointer table
complete, `glxgears` gave 430 FPS (Intel) and SuperTuxKart 93 FPS (Intel) and 1273 FPS (AMD).

**Findings worth knowing.** Stock Mesa rejects the internal sRGB fourcc that the original
redirection used for its buffers; the library falls back to `XRGB8888` (an sRGB default
framebuffer may render with different colours). Mesa duplicates the DRM fd with
`fcntl(F_DUPFD_CLOEXEC)`: the host's returned descriptor number must be kept, or Mesa cannot
query the device. A graphics-enabled LibOS crashed applications without graphics that touched
`/dev` or `/sys`, because redirection was not gated on the communication page being mapped. The
communication ABI (the page layout, request ids, the fd offset) was triplicated across the LibOS,
the library and the QEMU fork; it is now one header shared by the three. The QEMU fork is a
recent upstream QEMU plus four files for the listener and identity-mapped guest RAM.

**Limits and open items.** Mouse look in Quake II is slower than natively (relative motion goes
over the X relay, not the ring). Minetest with one vCPU is far below native; multi-vCPU runs are
few. One presenter (one listener window) per guest, and no GEM clean-up when a process dies.
Team Fortress 2 is set up but needs Steam. The listener and the fixed communication-page address
are x86-specific, so AArch64 has no VM route. Two graphics VMs share one X relay; the second loses
its display when the first exits.
