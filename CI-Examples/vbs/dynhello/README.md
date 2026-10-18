# Dynamically linked program (VBS enclave)

A dynamically linked program. The LibOS loads it and `ld.so`, and `ld.so` maps `libc.so.6`. The
program sorts an array with `qsort` and prints the result; `make VBS=1 check` looks for the
lines `sorted 1 3 5 7 9` and `cos(0) = 1`. The example is for the VBS backend only.

```sh
make VBS=1
gramine-vbs dynhello
```

## Running under each backend

Build first (`make`, or `make VBS=1` for vbs) and activate the install of the backend (`Documentation/quickstart/`). Results and dates are in the matrix of `CI-Examples/README.md`.

- **direct, vm, sev, tdx, sgx:** not applicable. The example is for the VBS backend only (`make` without `VBS=1` says so).
- **vbs:** `make VBS=1 check`. Verified on ARM64 and x64.
