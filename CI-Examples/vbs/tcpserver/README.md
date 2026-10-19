# TCP server (VBS enclave)

A TCP server in the enclave. It listens on 127.0.0.1:18080 through the host's sockets, answers
three clients with `echo from the enclave: <request>` and exits. `client.ps1` is the client for
PowerShell on Windows. The example is for the VBS backend only.

```sh
make VBS=1
gramine-vbs tcpserver
```

Then connect from another PowerShell window, three times: `.\client.ps1`.

## Running under each backend

VBS only: `make VBS=1` builds, `make VBS=1 check` runs it and checks the output. The other backends do not apply (`make` without `VBS=1` says so). Status: matrix in [`CI-Examples/README.md`](../../README.md).
