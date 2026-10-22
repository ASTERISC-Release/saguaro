# Quickstart: graphics under `gramine-direct` (x86_64 and AArch64)

Under `gramine-direct` an OpenGL (and, on x86_64, Vulkan) application uses the host's GPU
directly: its render node or the NVIDIA driver's device nodes, the host's X server, and the
host's Mesa or NVIDIA libraries. No QEMU and no preload library are involved. Details:
`dev docs: graphics-direct/running.md` and
`dev docs: graphics-aarch64/`.

## Requirements

- **x86_64**: an Intel or AMD GPU with Mesa, a render node (`/dev/dri/renderD128`) that the user can
  open, and an X server (Xwayland counts) with `DISPLAY` set.
- **AArch64**: an NVIDIA GPU with the proprietary driver (`/dev/nvidiactl`), and an X server on
  that driver, for example the display of the desktop session (`:1` on a GDM login). The
  session may be locked; frames are still drawn.

## 1. Build (in the container)

Create the container in a shell where `DISPLAY` is set: the container then gets the X socket and
the render nodes. On AArch64 with NVIDIA also set `SAGUARO_CONTAINER_NVIDIA=1`, which passes the
NVIDIA device nodes and mounts the host's NVIDIA libraries (they must match the kernel module):

    container/run.sh                                     # x86_64
    DISPLAY=:1 SAGUARO_CONTAINER_NVIDIA=1 container/run.sh   # AArch64 with NVIDIA

These variables only take effect when the container is created; an existing container of the
checkout has to be removed first (`container/run.sh down`, which discards the install inside
it). Then, inside:

    scripts/install.sh                 # the direct PAL
    . ~/.local/saguaro/env.sh
    mkdir -p ~/.cache                  # the -direct manifests mount it

On AArch64 the container image lacks the games' build dependencies; inside the container (as its
root user) install at least:

    apt-get update && apt-get install -y libsdl2-dev libopenal-dev libcurl4-openssl-dev

The graphics examples are started by hand (their `exec.sh` lists no backend and prints "needs a
display; run by hand"), so `./exec.sh` does not apply to them.

## 2. glxgears

    cd CI-Examples/graphics/glxgears
    make glxgears
    make run-direct GLXGEARS_ARGS="300 300 -stats-interval 5"

A 300x300 window with the gears opens and the program prints frame-rate statistics every 5
seconds. Stop it with Ctrl-C, or run it as `timeout -s TERM 20 make run-direct ...` (the
manifests let `SIGTERM` through, so the program exits cleanly). On a machine with
`/dev/nvidiactl` the Makefile sets `NVIDIA=1` by itself, which adds the NVIDIA device nodes and
`/proc/driver/nvidia` to the manifest; `NVIDIA=0` or `NVIDIA=1` overrides it.

## 3. Quake II

    cd CI-Examples/graphics/quake2
    ./setup-quake.sh                   # once: builds Yamagi Quake II, fetches the demo's data
    make run-direct-timedemo           # plays the demo as fast as it can, then waits at the console
    make run-direct                    # the game

The timedemo prints `633 frames, N seconds: N fps` and then stays at the game's console; stop
it with `timeout -s TERM`, as above, or the game's `quit` command. A fresh configuration
starts in full screen at 640x480.

## 4. Vulkan (x86_64)

Vulkan programs run as they are, on the host's Vulkan drivers:

    cd test/vulkan
    make direct-vulkaninfo.manifest direct-vkcube.manifest
    gramine-direct direct-vulkaninfo --summary     # lists the GPUs
    gramine-direct direct-vkcube                   # the spinning cube, until stopped

## Per-architecture notes

- **Which GPU (x86_64)**: Mesa renders on the X server's GPU. `DRI_PRIME=pci-0000_03_00_0` (the
  name from `ls /dev/dri/by-path`) selects another one; the manifests pass the host
  environment.
- **NVIDIA (AArch64)**: the driver passes file descriptors inside its ioctls; Saguaro translates
  the ones of the driver's known ioctls (and those the X server sends back) to the application's
  own files, but does not check the requests themselves. The driver ignores `vblank_mode`; the
  manifests set `__GL_SYNC_TO_VBLANK=0`.
- **Software rendering** as a fallback (no usable GPU, or to compare): on AArch64 with NVIDIA,
  `__GLX_VENDOR_LIBRARY_NAME=mesa LIBGL_ALWAYS_SOFTWARE=1 make run-direct ...` uses Mesa's
  `llvmpipe`; on x86_64 `LIBGL_ALWAYS_SOFTWARE=1`.
- **The X display**: the manifests are rendered for the display of `$DISPLAY` in the shell that
  runs `make` and name its socket `/tmp/.X11-unix/X<N>`. `Mounting "file:/root/.cache" ... failed` means the `mkdir -p ~/.cache` step was skipped.

More games (SuperTuxKart, Minetest) and their targets: [CI-Examples/graphics/README.md](../../CI-Examples/graphics/README.md).

Not verified: stopping with Ctrl-C or the game's `quit`; `make run-direct` of Quake II on
AArch64 (only its timedemo); Vulkan on AArch64 (not part of this quickstart).
