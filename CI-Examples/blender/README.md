# Blender

This directory contains a Makefile and template manifest to run Blender v2.82 on
Gramine.

# Quick Start

To run the test, execute `make check`. For SGX, execute `make SGX=1 check`.

# Running under each backend

Build and run in this directory, with the install of the backend active (`Documentation/quickstart/`). Pass or fail status: matrix in [`CI-Examples/README.md`](../README.md).

- **direct:** `make check` (x86_64 only: the example downloads an x86_64 binary).
- **vm, sev:** no `make` target: `test_all_scenes.sh` runs only `gramine-direct` (or `gramine-sgx`).
- **vbs:** no recipe.
