# Blender

This directory contains a Makefile and a template manifest to run Blender 2.82 on Gramine. Pass or
fail status per backend: matrix in [`CI-Examples/README.md`](../README.md).

# Building

Install the backend first (`Documentation/quickstart/`) and run all commands in this directory.
The Makefile downloads an x86_64 Blender binary, so the example runs on x86_64 only.

## Direct

```sh
make
```

## SGX

```sh
make SGX=1
```

The example has no VBS recipe. VM and SEV: the Makefile has no VM or SEV target.

# Testing

```sh
make check
```

This renders every scene in `data/scenes/` with `gramine-direct` (`SGX=1 make check` uses
`gramine-sgx`) and checks that the image `data/images/<scene>0001.png` exists. The script
`test_all_scenes.sh` hardcodes `gramine-direct` and `gramine-sgx`, so `make check` does not take
`GRAMINE=gramine-vm` or `GRAMINE=gramine-sev`.
