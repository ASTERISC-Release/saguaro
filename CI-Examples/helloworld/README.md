# Hello World

This directory contains a Makefile and a manifest template for running a simple "Hello World"
program in Gramine. It serves as a sanity test of a Gramine installation. Pass or fail status per
backend: matrix in [`CI-Examples/README.md`](../README.md).

# Building

Install the backend first (`Documentation/quickstart/`) and run all commands in this directory.
`make DEBUG=1` builds with debug logging.

## Direct, VM, and SEV

```sh
make
```

## SGX

```sh
make SGX=1
```

## VBS

In WSL on a Windows machine with VBS (`dev docs: vbs-enclave/manual.md`, "Makefile flow"). The command builds
the program as a static PIE against the vbs glibc (in `vbs-build/`), packs it into
`helloworld.vbs/` and links and signs `helloworld.dll` on Windows:

```sh
make VBS=1
```

# Testing

The program prints `Hello, world`:

```sh
gramine-direct helloworld
```

The other backends run the same program with their own command:

```sh
gramine-vm helloworld                            # VM
GRAMINE_RAM_SIZE=4G gramine-sev helloworld       # SEV (Documentation/quickstart/sev.md)
gramine-sgx helloworld                           # SGX; not verified
gramine-vbs helloworld                           # VBS (scripts/vbs/gramine-vbs)
```

`make check` runs the program with the backend in `GRAMINE` and compares its output
(`make check GRAMINE=gramine-vm` for the VM; `make VBS=1 check` for VBS). SEV needs
`GRAMINE_RAM_SIZE=4G` as shown. No backend needs another setting.
