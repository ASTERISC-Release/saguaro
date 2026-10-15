# Bastion

Supergiant Games' Bastion, the Linux build sold on GOG.com (MonoKickstart: a bundled Mono runtime,
FNA on SDL2 and OpenGL, FMOD for audio), under `gramine-vm` with the graphics host redirection
(x86_64). General setup, host requirements and caveats: `../README.md` and
`Documentation/devel/graphics/`.

**Status (2026-10-15, KVM, `make run`, nobody watched the screen; checked with X screenshots):**

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
```

Variables: `SHARED_INPUT=0` (input over the X connection), `SYSTEM_SDL=0` (use the game's bundled SDL2
instead of the host's, which is the default so that the graphics preload library interposes the system
library), `DATA_DIR` (host directory for save games and settings, default
`~/.local/share/gramine-bastion`), `DEBUG=1`.

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
