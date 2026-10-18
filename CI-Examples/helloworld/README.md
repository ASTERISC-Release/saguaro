# Hello World

This directory contains a Makefile and a manifest template for running a simple "Hello World"
program in Gramine. It serves as a sanity test of a Gramine installation.

# Building

## Building for Linux

Run `make` (non-debug) or `make DEBUG=1` (debug) in the directory.

## Building for SGX

Run `make SGX=1` (non-debug) or `make SGX=1 DEBUG=1` (debug) in the directory.

## Building for a Windows VBS enclave

In WSL on an ARM64 Windows machine with VBS (see `Documentation/vbs.md`, "Makefile flow"), run
`make VBS=1` in the directory: it builds the program as a static PIE against the vbs glibc (in
`vbs-build/`), renders `vbs.manifest.template` into `helloworld.manifest.vbs`, packs it into
`helloworld.vbs/` and links and signs `helloworld.dll` on Windows. `make VBS=1 check` runs it.

# Run Hello World with Gramine

Without SGX:
```sh
gramine-direct helloworld
```

With SGX:
```sh
gramine-sgx helloworld
```

In a VBS enclave (`scripts/vbs/gramine-vbs` of the repository):
```sh
gramine-vbs helloworld
```

# Running under each backend

Build first (`make`, or `make VBS=1` for vbs) and activate the install of the backend (`Documentation/quickstart/`). Results and dates are in the matrix of `CI-Examples/README.md`.

- **direct:** `gramine-direct helloworld`. Verified on x86_64 and aarch64.
- **vm:** `gramine-vm helloworld`. Verified on x86_64 and aarch64.
- **sev:** `GRAMINE_RAM_SIZE=4G gramine-sev helloworld`. Verified.
- **tdx:** reported by the maintainer to pass, offline, as under SEV.¹
- **sgx:** not verified.² The commands are given above.
- **vbs:** `gramine-vbs helloworld`. Verified on ARM64 and x64.

¹ TDX was tested offline by the maintainer, with the same result as SEV. No TDX log is in the repository, so the result is reported and not verified here. The TDX install needs `td-shim` (`Documentation/TODO.md`).  
² SGX has not run on this tree's hardware. The SGX build needs `protobuf-c` (`Documentation/TODO.md`).
