# Team Fortress 2

The native Linux TF2 client under `gramine-vm`, ported from Tirith. It targets an insecure,
LAN-only local server on `cp_badlands`. `-insecure` deliberately disables VAC; do not use this
launch for public matchmaking.

**Status: ported, not yet run in this tree.** The scripts, manifest and workload configuration are
adapted to this repository, the frame-rate collector builds, and the manifest renders; the game
itself has not been downloaded or started here. Everything below about behaviour is what Tirith
documents, not something verified in this tree.

## What was changed from Tirith

- The preload library is the installed `libgraphics.so` (`{{ gramine.libgraphics }}`), and the
  manifest uses stock Mesa; the `/opt/mesa` mounts are gone.
- `fps_metrics.c`, the preloaded frame-time collector, lives in this directory and is built by the
  Makefile (in Tirith it was part of `egl-redirect`).
- The Steam relay script is `./steam-proxy.sh` (Tirith: `scripts/host-steam-proxy.sh`).
- `setup-tf2.sh` no longer installs packages with `sudo`.
- Input goes over the X connection by default, as in Tirith (`SHARED_INPUT=0`). The game ships its
  own SDL2; whether that works with the shared input ring is untested. `make SHARED_INPUT=1` tries
  the ring.
- Not ported: the muvm and VirGL launchers, the cross-runtime benchmark matrix
  (`run-benchmarks.sh`) and `xinitthreads.c`, which only those need.

## What a run needs

1. **The game.** TF2 is free-to-play, but its depots need a Steam account that has claimed the
   free license: `./setup-tf2.sh STEAM_USERNAME` downloads about 30 GiB into `./game` with
   SteamCMD, which prompts for the password and Steam Guard code (never pass a password on the
   command line). An existing Linux install can be used instead with
   `make TF2_DIR="$HOME/.local/share/Steam/steamapps/common/Team Fortress 2"`.
2. **A logged-in desktop Steam client on the host.** The client talks to it over local IPC even
   for this offline match.
3. **The Steam relay.** `gramine-vm` starts the X relay itself, but this one is manual:
   `./steam-proxy.sh` forwards guest vsock port
   57343 to the Steam client on `127.0.0.1:57343`.
4. **`xdotool`**, used by `auto-input.sh` to get through the Welcome and class-selection panels
   (`TF2_AUTOINPUT=0` disables it).

Then:

```sh
make
make run                         # GRAMINE_RAM_SIZE=16G GRAMINE_CPU_NUM=1 ./run-vm.sh
./run-vm.sh --res=1280x720 --bots
./run-native.sh 1 1              # without Gramine: <graphics library 0|1> <pin to one CPU 0|1>
```

## Open points for this tree

Tirith ran this in a privileged container that shared the host's PID and IPC namespaces and
mounted `~/.steam` and `~/.local/share/Steam` at their original paths. The container here is
rootless and does none of that, so before the first run these have to be settled:

- **Steam's IPC identifies the game by PID**, so Tirith needed the host PID namespace. Whether
  this can work from a rootless container, or the game has to be launched from the host, is open.
- **The manifest enables `sys.experimental__enable_host_sysv_semaphores`**, which forwards SysV
  semaphores to the host for Steam's client protocol; that needs the host IPC namespace.
- **The Steam relay has to reach the host's loopback** (`127.0.0.1:57343`), which a rootless
  container's network namespace does not share; the relay probably has to run on the host.
- **The Steam directories are not mounted** into the container, and `xdotool` is not in the image.

The game-specific notes from Tirith (the automatic input sequence, the fixed benchmark view,
`r_lightmap_bicubic 0`, the frame-rate collector's warm-up) are in the scripts' comments and in
Tirith's README.
