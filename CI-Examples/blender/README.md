# Blender

This directory contains a Makefile and a template manifest to run Blender 2.82 on Gramine. Pass or
fail status per backend: matrix in [`CI-Examples/README.md`](../README.md).

# Building

Install the backend first (`Documentation/quickstart/`) and run all commands in this directory.
The Makefile downloads an x86_64 Blender binary, so the example runs on x86_64 only.
`./install-prereqs.sh` installs `lsb_release`, which the Makefile uses.

## Direct, VM, and SEV

```sh
make
```

## SGX

```sh
make SGX=1
```

The example has no VBS recipe.

# Testing

```sh
make check
```

This renders every scene in `data/scenes/` with `gramine-direct` (`SGX=1 make check` uses
`gramine-sgx`) and checks that the image `data/images/<scene>0001.png` exists. `GRAMINE` selects
another runner: `make check GRAMINE=gramine-vm`, or `./exec.sh --vm` (also `--direct` and `--sev`).
