# Bastion

Supergiant Games' Bastion, the Linux build sold on GOG.com (MonoKickstart: Mono, FNA on SDL2 and OpenGL), under `gramine-direct` on the host GPU or under `gramine-vm` with the graphics redirection (x86_64).

## Prerequisites

The game is commercial and not in this repository. Buy the Linux installer on GOG.com (<https://www.gog.com/game/bastion>), then unpack its game files into `./bastion_game` (about 1.3 GB, no root; the installer is not run):

```sh
./setup-bastion.sh [path/to/bastion_<version>.sh]   # default: ~/Downloads/bastion_*.sh
```

Backend and display setup: [`../README.md`](../README.md).

## Run

```sh
make run-direct      # gramine-direct on the host GPU with the host's Mesa
make run             # gramine-vm (QEMU_PATH exported, see ../README.md)
make run-native      # without Gramine, with the preload library
```

## Notes

- `DATA_DIR` (default `~/.local/share/gramine-bastion`) holds save games and settings. `BASTION_ARGS` (default `-windowed`) and `PRELOAD=1` apply to `run-direct`. `SYSTEM_SDL=0` uses the game's bundled SDL2. `SHARED_INPUT=0` takes input over the X connection.
- `MULTIPROC=1` adds `sys.experimental__multiprocess = true` and a `/bin` mount (FMOD runs `/bin/sh`).
- Without a profile the game draws at the desktop size; set Help & Options, Resolution, 1920x1080 in the game (stored in `DATA_DIR`) if a smaller window shows only a corner.
