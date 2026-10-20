# TCP server (VBS enclave)

A TCP server in the enclave. It listens on `127.0.0.1:18080` through the host's sockets, answers
three clients with `echo from the enclave: <request>` and exits. `client.ps1` is the client for
PowerShell on Windows. The example is for the VBS backend only. Pass or fail status: matrix in
[`CI-Examples/README.md`](../../README.md).

# Building

Install the VBS backend first (`dev docs: vbs-enclave/manual.md`) and run all commands in this directory.

## VBS

```sh
make VBS=1
```

The example does not run under direct, VM or SEV; `make` without `VBS=1` says so.

# Testing

```sh
make VBS=1 check
```

To run the server by hand, then connect from another PowerShell window, three times:

```sh
gramine-vbs tcpserver
```

```powershell
.\client.ps1
```
