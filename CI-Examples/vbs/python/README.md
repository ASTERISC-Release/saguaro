# Python (VBS enclave)

A small Python script (`hello.py`) that prints the Python version, a SHA-256 digest and the result
of three threads. The check looks for `Python 3.12.7`. The example is for the VBS backend only. For the
full Python example see `../../python/README.md`.

```sh
make VBS=1
gramine-vbs python
```

## Running under each backend

Build first (`make`, or `make VBS=1` for vbs) and activate the install of the backend (`Documentation/quickstart/`). Results and dates are in the matrix of `CI-Examples/README.md`.

- **direct, vm, sev, tdx, sgx:** not applicable. The example is for the VBS backend only (`make` without `VBS=1` says so).
- **vbs:** `make VBS=1 check`. Verified on ARM64 and x64.
