# Graphics examples

OpenGL games and benchmarks that draw on the host GPU: under `gramine-direct` (x86_64, and AArch64 with the NVIDIA driver) or under `gramine-vm` with the graphics redirection (x86_64). They need a display, so `exec.sh` runs none of them: run them by hand.

## Prerequisites

Install the backend, with graphics, and set `DISPLAY`: [`Documentation/quickstart/graphics-direct.md`](../../Documentation/quickstart/graphics-direct.md) and [`Documentation/quickstart/graphics-vm.md`](../../Documentation/quickstart/graphics-vm.md). Then run the setup script of the application, if it has one (once, from its directory):

| Directory | Application | Setup |
|---|---|---|
| `glxgears` | glxgears with frame-rate statistics | none |
| `quake2` | Yamagi Quake II with the demo data | `quake2/setup-quake.sh` |
| `minetest` | Minetest 5.6.1 | `minetest/setup-minetest.sh` |
| `supertuxkart` | SuperTuxKart | `supertuxkart/install-stk.sh` |
| `bastion` | Bastion (a game you buy) | see `bastion/README.md` |
| `tf2` | Team Fortress 2 (VM only) | see `tf2/README.md` |

## Run

```sh
cd CI-Examples/graphics/<example>
mkdir -p ~/.cache
make run-direct    # gramine-direct on the host GPU (not tf2)
make run           # gramine-vm
make run-native    # without Gramine, to tell a GPU problem from a Gramine one
```

For the VM, export `QEMU_PATH=<qemu checkout>/build/qemu-system-x86_64` first. The QEMU fork is a separate checkout, built as described in [`graphics-vm.md`](../../Documentation/quickstart/graphics-vm.md).

## Notes

- `DEBUG=1` sets the Gramine debug log level; `SHARED_INPUT=0` takes input over the X connection instead of the shared ring.
- `GRAMINE_CPU_NUM` and `GRAMINE_RAM_SIZE` set the guest's vCPUs and memory (defaults 1 and 8G). A graphics guest needs at least 5G, and 3G plus `GRAMINE_DATA_SIZE` (default 1G).
- Quake II: `make run-direct-timedemo`. SuperTuxKart: `make run-startup PROFILE_TIME=<seconds>`, `make run-direct-startup`.
- The `-direct` manifests enable insecure options (raw GPU ioctls, the X server's Unix socket): `gramine-direct` is not a sandbox here.
- `direct-tests/` holds small test programs for the direct route (`make check`).
