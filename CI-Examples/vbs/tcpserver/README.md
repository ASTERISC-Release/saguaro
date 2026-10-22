# TCP server (VBS enclave)

A TCP server that listens on `127.0.0.1:18080` through the host's sockets, answers three clients with `echo from the enclave: <request>` and exits. VBS only.

## Run

In WSL on a Windows machine with VBS enabled. Install the backend first: [`Documentation/quickstart/vbs-windows.md`](../../../Documentation/quickstart/vbs-windows.md) (machine setup: [`vbs-enable.md`](../../../Documentation/quickstart/vbs-enable.md)).

```sh
make VBS=1              # build and sign
make VBS=1 check        # run and check
saguaro-vbs tcpserver   # by hand; then run .\client.ps1 three times in PowerShell
```

`../exec.sh --vbs` builds, runs and checks all the VBS examples; `make` without `VBS=1` says that the example does not run elsewhere.
