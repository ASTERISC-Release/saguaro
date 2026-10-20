# Hello World (VBS enclave)

A program that prints `Hello, world from a VBS enclave`, its process id and the number of its
arguments. The example is for the VBS backend only. For the same program on the other backends see
`../../helloworld/README.md`.

```sh
make VBS=1
gramine-vbs helloworld
```

## Running under each backend

Build first (`make`, or `make VBS=1` for vbs) and activate the install of the backend (`Documentation/quickstart/`). Results and dates are in the matrix of `CI-Examples/README.md`.

- **direct, vm, sev, tdx, sgx:** not applicable. The example is for the VBS backend only (`make` without `VBS=1` says so).
- **vbs:** `make VBS=1 check`. Verified on ARM64 and x64.
