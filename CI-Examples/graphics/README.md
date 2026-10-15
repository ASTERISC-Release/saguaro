# Graphics examples

Games and benchmarks that run under `gramine-vm` with the graphics host redirection (x86_64), and
under `gramine-direct` on the host GPU (x86_64, and AArch64 with the NVIDIA proprietary driver). This page gives the general procedure for the VM;
details, caveats and results are in `Documentation/devel/graphics/` (`running.md`, `PLAN.md`,
`FINDINGS.md`, `TODO.md`, `TESTING.md`). For `gramine-direct` see the section at the end and
`Documentation/devel/graphics-direct/running.md`.

## Once per machine

Everything runs unprivileged inside the container (`container/run.sh` opens a shell in it).

```sh
/root/qemu-tirith/tirith-build.sh             # the QEMU fork with the graphics listener
scripts/install.sh --vm --graphics enabled    # Gramine; also builds libgraphics.so
```

`qemu-tirith` has to be checked out next to this repository before the container is created.

## Once per shell

```sh
. ~/.local/gramine-vm/env.sh
export QEMU_PATH=/root/qemu-tirith/build/qemu-system-x86_64
```

`gramine-vm` relays the application's X connection to the host X server by itself (it starts
`socat` for the run and stops it afterwards), so `DISPLAY` has to name a local X display in the
shell that runs it.

## Running an example

Each directory follows the same pattern:

```sh
cd CI-Examples/graphics/<example>
./setup-*.sh  (or ./install-*.sh)   # once: fetch and build the application, if it has one
make run-native                     # the application with the preload library, without Gramine
make run                            # the same under gramine-vm
```

Run `make run-native` first. It exercises the same preload library and GPU path without the VM,
so a problem that shows up there is not a Gramine problem, and some games create their
configuration on that first run. Then compare with `make run`.

Common `make` variables:

| Variable | Meaning |
|---|---|
| `SHARED_INPUT=0` | receive input over the X connection instead of the shared ring |
| `DEBUG=1` | Gramine debug log level |

Environment: `EGL_DISCRETE=1` selects `/dev/dri/renderD129` instead of `renderD128` for
`run-native`; for the VM it is set in the example's manifest template. `GRAMINE_CPU_NUM` and
`GRAMINE_RAM_SIZE` set the guest's vCPUs and memory (defaults: 1 and 8G).

RAM floor and arena sizing: a graphics guest needs `3G + data-arena size`, and at least 5G
(the launcher enforces this for graphics manifests). The GEM data arena defaults to 1G; export
`GRAMINE_DATA_SIZE=3G` (any G/M value or byte count) before `make run` to enlarge it -- the
launcher relays the value to the guest, which relocates the hugepage arena accordingly. Larger
arenas raise the floor accordingly (e.g. 3G wants 6G of guest RAM).

## Measuring

Run the same workload natively and under the VM, at the same window size and settings, and compare
the frame rates the application prints. Where a game has no fixed benchmark of its own, the example
adds a frame-rate report (average, 1% low) to it. Settings matter more than anything else: start
from moderate settings with a frame cap on an integrated GPU, and note the GPU, resolution and
vCPU count with every number.

## The examples

| Directory | Application | Self-terminating benchmark |
|---|---|---|
| `glxgears` | glxgears with frame-rate statistics | no (prints a report every minute) |
| `supertuxkart` | SuperTuxKart | `make run-startup PROFILE_TIME=<seconds>`, `make run-native-startup` |
| `quake2` | Yamagi Quake II with the demo's game data | no |
| `minetest` | Minetest 5.6.1 | no (prints a report every minute) |
| `bastion` | Bastion (the GOG.com Linux build; you buy the game) | no; runs under `gramine-direct`, not yet run under `gramine-vm`; see its `README.md` |
| `tf2` | Team Fortress 2 | ported, not yet run; see its `README.md` |

## Adding an application

Copy the Makefile and manifest template of the closest example. The manifest has to mount the
preload library at `/libgraphics.so` (`{{ gramine.libgraphics }}`) and preload it, set
`EGL_GRAMINE=1` and `DISPLAY=2:0` (which is also what makes `gramine-vm` start the X relay), and mount `/usr`, `/sys` and `/root/.cache` as the examples do.

## Under gramine-direct

`glxgears`, `quake2`, `minetest`, `supertuxkart` and `bastion` also have a `<name>-direct.manifest.template` and a
`make run-direct` target (Quake II: `make run-direct-timedemo`; SuperTuxKart:
`make run-direct-startup`). The application then runs as a Linux process on the host's GPU and X
server, with the host's Mesa (DRI3), no QEMU and no preload library; `PRELOAD=1` uses the preload
library instead. The manifests enable three insecure options (raw GPU ioctls, the X server's Unix
socket, host symbolic links under `/sys`): `gramine-direct` is not a sandbox, see
`Documentation/devel/graphics-direct/running.md`. Use an install made with `scripts/install.sh`
(direct PAL, bundled glibc). The `-direct` manifests mount `~/.cache`: create it first (`mkdir -p
~/.cache`) in a fresh container. `direct-tests/` has test programs (`make check`).


On AArch64 with the NVIDIA proprietary driver (`Documentation/devel/graphics-aarch64/`), the same
`-direct` targets of `glxgears`, `quake2`, `minetest` and `supertuxkart` run on the GPU; the manifests
then also mount the NVIDIA device nodes (automatic when `/dev/nvidiactl` exists). The games are built
from source in the container. `bastion` and `tf2` are x86_64 binaries and do not apply there.
