# Node.js

This directory contains a Makefile and a template manifest to run Node.js on Gramine. The example
uses the Node.js installed on the system and does not compile it from source. Pass or fail status
per backend: matrix in [`CI-Examples/README.md`](../README.md).

# Building

Install the backend first (`Documentation/quickstart/`) and run all commands in this directory.

## Direct, VM, and SEV

```sh
make
```

## SGX

```sh
make SGX=1
```

## VBS

In WSL on a Windows machine with VBS (`Documentation/vbs.md`, "Makefile flow"). The recipe downloads
and packs the linux-arm64 `node`, so it applies to ARM64:

```sh
make VBS=1
```

# Testing

`make check` runs `helloworld.js` and prints `[ Success 1/1 ]` if the output contains `Hello World`.
It takes the backend from `GRAMINE`:

```sh
make check                          # direct
make check GRAMINE=gramine-vm       # VM; not verified
make check GRAMINE=gramine-sev      # SEV; not verified
make SGX=1 check                    # SGX; not verified
make VBS=1 check                    # VBS
```

No backend needs another setting in the Makefile or the manifest template.
