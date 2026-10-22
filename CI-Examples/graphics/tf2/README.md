# Team Fortress 2

The native Linux Team Fortress 2 client under `saguaro-vm` with the graphics redirection (x86_64), on an insecure LAN-only local server (`cp_badlands`). `-insecure` disables VAC: do not use it for public matchmaking. There is no `run-direct` target.

## Prerequisites

- The game: `./setup-tf2.sh STEAM_USERNAME` downloads about 30 GiB into `./game` with SteamCMD (it needs a Steam account that has claimed the free license and prompts for the password). To use an existing install: `make TF2_DIR="<path to Team Fortress 2>"`.
- A logged-in desktop Steam client on the host, and `./steam-proxy.sh` running, which forwards guest vsock port 57343 to the Steam client on `127.0.0.1:57343`.
- `xdotool`, which `auto-input.sh` uses to get through the menus (`TF2_AUTOINPUT=0` disables it).
- The graphics VM setup of [`../../../Documentation/quickstart/graphics-vm.md`](../../../Documentation/quickstart/graphics-vm.md), with `QEMU_PATH` exported.

## Run

```sh
make
make run                         # SAGUARO_RAM_SIZE=16G SAGUARO_CPU_NUM=1 ./run-vm.sh
./run-vm.sh --res=1280x720 --bots
./run-native.sh 1 1              # without Gramine: <graphics library 0|1> <pin to one CPU 0|1>
```

## Notes

- Input goes over the X connection by default; `make SHARED_INPUT=1` uses the shared input ring.
- The manifest enables `sys.experimental__enable_host_sysv_semaphores` (Steam's client protocol uses SysV semaphores).
