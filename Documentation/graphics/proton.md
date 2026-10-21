# Wine/Proton under `gramine-vm`

> All numbers on this page are experimental measurements from one machine, taken during development; they are not benchmarks.

**What it does.** GE-Proton's Wine (Wine 11 with DXVK and VKD3D-Proton), driven without the Steam
client through Proton's own environment, runs Windows programs and games inside the `gramine-vm`
microVM, first with software rendering (lavapipe), then on the host GPU through the redirected
route ([vm.md](vm.md), [vulkan.md](vulkan.md)). A feasibility study with a reduced scope: 64-bit
first, server-side synchronisation, then the GPU, then 32-bit games.

**How it works.** A native Proton run was traced and compared with the VM PAL, which gave the
blocker list. The LibOS then gained, in order: named Unix-socket nodes (Wine finds its server),
a `fork` that sends the VMAs instead of copying untouched reservations, `SCM_RIGHTS` fd passing
between Gramine processes, a virtual user `%gs` handed to the application lazily at the LibOS
boundary, interruptible blocking calls and the exception vectors as signals, a demand-paged
address space of 512 GiB per process with on-demand page tables and free `PROT_NONE`
reservations, syscall user dispatch and drive links. *GPU design:* the redirection had required an
identity-mapped process, which Wine's layout cannot live in; the design chosen keeps every process
demand-paged and lets the LibOS copy each forwarded ioctl argument and the arrays it points to into
physically contiguous bounce buffers, rewriting the pointers to physical addresses (a per-request
table; `AMDGPU_CS`, the i915 extension chains and `I915_QUERY` in code). One graphics session per
guest is shared by its processes through guest-private state in the communication page. *32-bit:*
Wine's new WoW64 mode runs 32-bit games in a 64-bit process that jumps into a 32-bit code
segment; the VM's GDT was laid out with Linux's selector numbers, per-thread TLS descriptors live
in a per-CPU GDT and are set through `set_thread_area` (also via `int $0x80`), signals landing in
32-bit code enter the 64-bit handler, and `modify_ldt` stays unimplemented (Wine's fallback only).

**Key results.** `hello.exe` prints its six lines in an 8 GB guest; `d3d11.exe` (Direct3D 11 through
DXVK) renders on the AMD GPU in the guest at about 3,700 FPS; `vkwin.exe` presents through the
layer. DOOM I Enhanced (64-bit, Direct3D 11): software rendering reached the first level at 44 to
48 FPS; on the GPU (4 vCPUs, 12 GB) it plays the first level on the Intel iGPU at
59.5 FPS against 68 to 69 natively (vsync off, 86 %) and on the AMD GPU at 72 FPS, bound by
the present path (about 70 presents/s) against 2300 natively. Psychonauts, The Witcher: Enhanced
Edition and BioShock 2 (32-bit, Direct3D 9) start on both GPUs (intro, title screen, main menu);
Tomb Raider: Underworld does not load. The table with every number is in
[../GRAPHICS.md](../GRAPHICS.md).

**Findings worth knowing.** Mesa's XShm image, a System V segment whose backing reached the host
through a GEM arena, stalled the Witcher; the layer created 1x1 swapchains because the X window was
1x1 when the swapchain was made; Vulkan memory that WoW64 places below 4 GB with `MAP_FIXED` was
left unbacked by the GPU buffer (black frames) until placed mappings alias the arena slice; the
100 ms timer tick lost wake-ups under host load. A 260-byte `memmove` in Wine's Unix side reads
past the end of its mapping; it crashed DOOM on the AMD path while the LibOS's own mappings
interleaved with the application's ([../GRAPHICS.md](../GRAPHICS.md), the guard). Tomb Raider's
executable has no relocations and must load at a fixed low range that the guest does not leave
free.

**Limits and open items.** No audio device in the guest; networking is loopback only (online
features hang); Steam-API games need the client or a stub; the guest's exit code is not reported
and the wineserver lingers; relative mouse and raw input are untested in a game; sound, gameplay
beyond the first level or menu, and equal-scene comparisons for the 32-bit games remain to be
done. The D3D11 pacing on Intel, `fsync`, and the cost of the `%gs` swap are open.
