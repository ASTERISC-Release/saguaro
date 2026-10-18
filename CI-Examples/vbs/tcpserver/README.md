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

Build first (`make`, or `make VBS=1` for vbs) and activate the install of the backend (`Documentation/quickstart/`). Results and dates are in the matrix of `CI-Examples/README.md`.

- **direct, vm, sev, tdx, sgx:** not applicable. The example is for the VBS backend only (`make` without `VBS=1` says so).
- **vbs:** `make VBS=1 check`. Verified on ARM64 and x64.
