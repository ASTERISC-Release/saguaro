# Quickstart: graphics under `gramine-vm` (x86_64)

With the graphics host redirection ([Tirith](https://arxiv.org/abs/2609.17525)) an OpenGL or
Vulkan application runs inside the `gramine-vm` microVM and draws on the host's GPU: a preload
library in the guest (`libgraphics.so`) forwards the GPU work to a listener in a QEMU fork on the
host, and the application's own X connection is relayed over vsock. Details:
`dev docs: graphics/running.md` and
`dev docs: vulkan/`.

Status (as in [README.md](../../README.md)): Intel x86_64 ⚠️, AMD64 ⚠️ (experimental);
AArch64 🛫 (not available: the QEMU fork does not build for AArch64); RISC-V ❌.

## Requirements

- An x86_64 machine with KVM (`/dev/kvm`), `/dev/vhost-vsock`, and an Intel or AMD GPU with a
  render node (`/dev/dri/renderD128`, `renderD129`) that the user can open.
- An X server (Xwayland counts) with `DISPLAY` set when the container is created.
- The QEMU fork [qemu-tirith](https://github.com/adilahmad17/qemu-tirith), branch
  `dev-graphics`, checked out **next to** this repository before the container is created (or
  `QEMU_TIRITH_DIR` set): `container/run.sh` mounts it at `/root/qemu-tirith`.

## 1. Build (in the container)

From the folder that holds the checkout:

    git clone -b dev-graphics https://github.com/adilahmad17/qemu-tirith.git
    cd gramine-multiarch
    container/run.sh                   # with DISPLAY set; the rest runs inside

Inside the container:

    /root/qemu-tirith/tirith-build.sh                  # once: build/qemu-system-x86_64
    scripts/install.sh --vm                            # graphics on by default; builds libgraphics.so
    export QEMU_PATH=/root/qemu-tirith/build/qemu-system-x86_64
    mkdir -p ~/.cache

`container/run.sh` loads `~/.local/gramine-vm/env.sh` in the container shell, so no environment step
is needed. The graphics examples are started by hand (their `exec.sh` lists no backend: it needs a
display), so `./exec.sh` does not apply to them.

If the container already existed without `DISPLAY` or without the fork, recreate it
(`container/run.sh down`, which discards the install inside it).

## 2. glxgears

    cd CI-Examples/graphics/glxgears
    make
    make run                           # gramine-vm glxgears

A window with the gears opens; the program prints a frame-rate report once a minute. Stop it with
Ctrl-C. `gramine-vm` starts and stops the X relay (`socat`) by itself. `make run-native` runs
the same preload library without the VM, to tell a GPU problem from a Gramine one.

## 3. Quake II (OpenGL and Vulkan)

    cd CI-Examples/graphics/quake2
    ./setup-quake.sh                   # once: the game and the demo's data
    make run                           # the game, OpenGL renderer
    make run VULKAN=1                  # the Vulkan renderer, presented through libgraphics.so
    EGL_DISCRETE=1 make run            # renderD129 (on a machine with two GPUs, the second one)

The benchmark (the demo's timedemo in a 1920x1080 window, then exit):

    test/vulkan/q2-timedemo.sh vm intel 1 gl3      # or: vm amd 1 vk, ...

prints `vm intel gl3 run 1: 633 frames, N seconds: N fps`.

## How the pieces fit

- **The manifest** mounts `libgraphics.so` at `/libgraphics.so` and preloads it, and sets
  `loader.env.DISPLAY = "2:0"`: the host's display 0 over vsock. That `DISPLAY` is what makes
  `gramine-vm` start the X relay; the shell that runs it needs a local `DISPLAY` and `socat`.
- **The GPU**: `EGL_DISCRETE` `"0"` opens `renderD128`, `"1"` `renderD129`; which
  physical GPU that is depends on the machine (`ls -l /dev/dri/by-path`).
- **Vulkan**: `libgraphics.so` also contains a presentation layer; `EGL_VULKAN_PRESENT` in the
  manifest turns it on (`test/vulkan/vk-vm.manifest.template` shows the keys, `make vm-<program>.manifest PRESENT=1` in `test/vulkan` builds one for a program in `/usr/bin`).
- **Memory**: a graphics guest needs at least 5G (the launcher enforces it; the default is 8G).

## Limits

- x86_64 only; graphics-enabled builds have only been run on `qemu-tirith`.
- Input arrives through a shared ring by default; applications that read input without the Xlib
  calls the ring covers need `SHARED_INPUT=0` (`running.md`).
- Two graphics VMs at once share one X relay; the second loses its display when the first exits.

## Verified

2026-10-15, `main` at `9e28ea81`, `qemu-tirith` `dev-graphics` at `45f0b236`, in the
container on an x86_64 host with KVM, an Intel UHD 770 (`renderD128`) and an AMD Radeon RX 9070
(`renderD129`), Xwayland: `tirith-build.sh` and `scripts/install.sh --vm`; glxgears `make run` (about 205 FPS over the first minute at the tiled window's size); `q2-timedemo.sh`: Intel
OpenGL 411 FPS and Vulkan 295 FPS, AMD OpenGL 3073 FPS and Vulkan 2131 FPS; `make run` of Quake
II opened its window with shared input enabled. Each window was closed after at most 70 seconds.

Not verified: `make run VULKAN=1` and `EGL_DISCRETE=1 make run` interactively (the same
manifest settings ran in `q2-timedemo.sh`); `make run-native`; stopping with Ctrl-C (the runs
were stopped with `timeout -s INT`); the `https` clone URL (the fork was cloned over ssh).
