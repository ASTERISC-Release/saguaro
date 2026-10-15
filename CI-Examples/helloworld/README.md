# Hello World

This directory contains a Makefile and a manifest template for running a simple
"Hello World" program in Gramine. It can be used as a sanity test for your
Gramine installation.

# Building

## Building for Linux

Run `make` (non-debug) or `make DEBUG=1` (debug) in the directory.

## Building for SGX

Run `make SGX=1` (non-debug) or `make SGX=1 DEBUG=1` (debug) in the directory.

## Building for a Windows VBS enclave

In WSL on an ARM64 Windows machine with VBS (see `Documentation/vbs.rst`, "Makefile flow"), run
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
