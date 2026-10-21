# Graphics: summaries of the development documents

> All numbers on this page are experimental measurements from one machine, taken during development; they are not benchmarks.

The graphics work of this tree is recorded in a private set of development documents (plans,
dated findings, test logs, open items), one topic each. The pages in this folder summarise them
for readers of the public repository: what each topic does, how it works, the key measured results,
the findings worth knowing, and the limits. The short overview, the known issue of the redirected
route and the Proton results are in [../GRAPHICS.md](../GRAPHICS.md); the guides are
[../quickstart/graphics-direct.md](../quickstart/graphics-direct.md) and
[../quickstart/graphics-vm.md](../quickstart/graphics-vm.md).

| Page | Topic | Summarises |
|---|---|---|
| [vm.md](vm.md) | redirected graphics for `gramine-vm` (x86_64) | the port of the graphics redirection: the preload library, the listener in the QEMU fork, the memory layout, the examples' frame rates |
| [direct.md](direct.md) | the host GPU under `gramine-direct` | render nodes and the X socket passed through, the ioctl translations, frame rates on Intel, AMD and NVIDIA |
| [aarch64.md](aarch64.md) | graphics on AArch64 | `gramine-direct` with the NVIDIA driver on an AArch64 board; why the VM route is not available there |
| [vulkan.md](vulkan.md) | Vulkan under both routes | the device path through forwarded ioctls, the presentation layer, the headless suite and the check script, Quake II and vkcube rates |
| [proton.md](proton.md) | Wine/Proton under `gramine-vm` | what Wine needed from the LibOS, the GPU design (bounce buffers and the pointer table), the 32-bit (WoW64) work, the games |

Ownership: each page belongs to the topic it summarises and is updated when that topic's
documents change. Measured numbers carry the date of the measurement; a number without a date
on these pages is the latest the topic's documents record.
