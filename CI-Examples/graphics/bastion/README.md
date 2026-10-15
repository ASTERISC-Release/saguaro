# Bastion

Supergiant Games' Bastion, the Linux build sold on GOG.com (MonoKickstart: a bundled Mono runtime,
FNA on SDL2 and OpenGL, FMOD for audio), under `gramine-vm` with the graphics host redirection
(x86_64), and under `gramine-direct` on the host GPU. General setup, host requirements and caveats:
`../README.md`, `Documentation/devel/graphics/` and `Documentation/devel/graphics-direct/`.

**Status under `gramine-direct`:** runs (2026-10-14, see "Under gramine-direct" below).

**Status under `gramine-vm` (2026-10-15, KVM, `make run`, nobody watched the screen; checked with X screenshots):**

| Configuration | Result |
|---|---|
| 2 vCPUs, `MULTIPROC=0` (default) | Starts, OpenGL through the redirection, reaches the title screen ("Press Any Key") in about 90 s. FMOD's `sh -c` probe fails (`process creation failed`) and the game continues without sound. Same as `main`. |
| 2 vCPUs, `MULTIPROC=1` | The game forks for FMOD's probe, but the `sh` child inherits `LD_PRELOAD=/libgraphics.so`, whose constructor repeats the comm-page handshake with the host and hangs, which disturbs the parent too: black window. With a guard in libgraphics (a child inheriting the graphics session skips it; `Documentation/devel/vm-multiproc/bastion-libgraphics-child-guard.patch`, not applied) the child exits 127 (`pulseaudio: not found`) and the title screen appears. |
| 1 vCPU, either setting | No window within 120 s: the game's GL goes to the host Mesa directly (`XDG_RUNTIME_DIR` error, the AMD GPU's name) instead of the redirected EGL path. `main` does the same, so it is not caused by the multi-process work; not diagnosed (graphics redirection). |

`MULTIPROC=1` adds `sys.experimental__multiprocess = true` and a `/bin` mount (FMOD runs `/bin/sh`).
Input, sound, saving and the resolution setting (the 1920x1080 crop) were not tested.

## The game is not included

Bastion is a commercial game. Buy it on GOG.com (<https://www.gog.com/game/bastion>) and download
the Linux installer (`bastion_<version>.sh`). The installer and the unpacked game files are never
committed (`.gitignore`).

```sh
./setup-bastion.sh [path/to/bastion_<version>.sh]   # default: ~/Downloads/bastion_*.sh
```

`setup-bastion.sh` does not run the installer. It extracts only the `data/noarch/game/` files of the
zip archive appended to it, into `./bastion_game` (about 1.3 GB, no root).

## Running

```sh
make run-native      # without Gramine, with the preload library (opens a window)
make run             # gramine-vm bastion (MULTIPROC=1: with the experimental processes)
make run-direct      # gramine-direct bastion-direct, on the host GPU with the host's Mesa
```

Variables: `SHARED_INPUT=0` (input over the X connection), `SYSTEM_SDL=0` (use the game's bundled SDL2
instead of the host's, which is the default so that the graphics preload library interposes the system
library), `DATA_DIR` (host directory for save games and settings, default
`~/.local/share/gramine-bastion`), `DEBUG=1`. For `run-direct` also `BASTION_ARGS` (the game's
options, default `-windowed`) and `PRELOAD=1` (the preload library instead of stock Mesa).

## Under gramine-direct, 2026-10-15 (Intel GPU, after merging main)

The game started under `gramine-direct` on the Intel GPU and drew its title screen,
cropped to a corner as in the native run below
(`Documentation/devel/graphics-direct/FINDINGS.md`). `SDL_VIDEO_X11_WMCLASS` sets the window's
class for a window rule.

## What is known

A native trace with `scripts/syscall-profile.py` (virtual display, software rendering, about a
minute, no input; `Documentation/devel/syscall-profile/TESTING.md`) shows:

- The game uses `memfd_create` and SysV shared memory (from Mesa and the X11 shared-memory path of the
  trace's virtual display), which the LibOS does not implement; the game ran unchanged with those
  syscalls answered by `-ENOSYS`.
- FMOD runs `sh -c "pulseaudio --check"`, which needs fork and fails in the single-process VM. Whether
  the game then runs without sound is not known.
- It uses 68 threads and about 1.4 million `futex` calls a minute: expect it to stress the VM's
  futex and timer paths.

Not checked: that the game starts under `gramine-vm`, rendering, input, audio, saving, and the
bundled SDL2 (`SYSTEM_SDL=0`).

## Native run, 2026-10-12 (`make run-native`, Hyprland, 3840x2160 desktop)

The game started with the graphics preload library, opened a window and (per the user) worked. With a
floating 1920x1080 window rule (see `Documentation/devel/graphics/TESTING.md`, "Window size") the picture
was **cropped to one corner**: the game asks for an 800x480 window, then sets a 3840x2160 drawable (its
default is the desktop resolution), which the rule squeezes to 1920x1080. Quake II had the same problem;
the fix there was the game's own windowed resolution. For Bastion that is Options, Display, Resolution
(1920x1080, fullscreen off); the setting is saved in the game's data folder (`DATA_DIR`). That was not
done yet: the game closed before the option was reached, so it is not known whether the setting
removes the cropping. `EGL_SDL_LOG=1` prints the window sizes the game requests.

## Under gramine-direct, 2026-10-14 (`make run-direct`, Hyprland, 3840x2160 desktop)

`bastion-direct.manifest.template` follows the other `-direct` manifests (raw GPU ioctls, the X
server's Unix socket, `/dev/dri`, `/dev/shm`, `/sys` with `host_symlinks`; see
`Documentation/devel/graphics-direct/running.md` for what these allow) and adds the game's `lib64`
to the library path, `/bin` (FMOD runs `sh -c "pulseaudio --check"`) and the save directories of `DATA_DIR`. Build with `scripts/install.sh`
(direct PAL, bundled glibc).

With the direct build of `c93cb62a` and the host's SDL2 (`SYSTEM_SDL=1`), the game started, FNA
opened OpenGL 4.6 on the AMD Radeon RX 6600 XT (radeonsi, Mesa 25.2.8), the main menu rendered
(checked with a screenshot), the user reported that the game works, and it exited normally (exit
code 0), having written its profile (`share/Bastion/Profile1.profile` in `DATA_DIR`). No sound: FMOD
finds no PulseAudio and no ALSA device (many `ALSA lib ...` lines on stderr), and the game runs on
without it. The bundled SDL2 (`SYSTEM_SDL=0`) was not tried.

The window opens at the desktop's size (3840x2160 here). Shrinking it to 1920x1080 (a floating
window set with `hyprctl`) **crops the picture to one corner**, as in the native run above: the game keeps
its 3840x2160 drawable. Set the game's own resolution (Help & Options) instead; it is saved in
`DATA_DIR`.
