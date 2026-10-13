# Bastion

Supergiant Games' Bastion, the Linux build sold on GOG.com (MonoKickstart: a bundled Mono runtime,
FNA on SDL2 and OpenGL, FMOD for audio), under `gramine-vm` with the graphics host redirection
(x86_64). General setup, host requirements and caveats: `../README.md` and
`Documentation/devel/graphics/`.

**Status: not yet run under `gramine-vm`.** The manifest renders and passes `gramine-manifest-check`;
the game was only traced natively (see below).

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
make run             # gramine-vm bastion
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
