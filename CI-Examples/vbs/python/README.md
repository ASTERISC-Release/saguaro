# Python (VBS enclave)

A small Python script (`hello.py`) that prints the Python version, a SHA-256 digest and the result
of three threads. The check looks for `Python 3.12.7`. The example is for the VBS backend only. For the
full Python example see `../../python/README.md`.

```sh
make VBS=1
gramine-vbs python
```

## Running under each backend

Run the commands in this directory after the build (`make`, or `make VBS=1`), with the install of the backend active (`Documentation/quickstart/`). The matrix in `CI-Examples/README.md` gives the result and the date for each backend. "Not verified" means that no result is recorded.

- **direct, vm, sev, tdx, sgx:** not applicable. The example is for the VBS backend only (`make` without `VBS=1` says so).
- **vbs:** `make VBS=1 check`. Verified on ARM64 and x64.
