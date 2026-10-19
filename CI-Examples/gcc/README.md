# Introduction

***TODO: this example should be reworked to use a standalone gcc version, not
the one from the system***

This directory contains a Makefile and template manifests to run gcc and its related tools on
Gramine. The example uses the packaged gcc and related tools (as, cc1, collect2, ld) installed on
the system and does not compile them from source.

# Quick Start

To run the regression tests, execute `make check`. For SGX, execute `SGX=1 make check`. The tests
build three sample programs (`helloworld.c`, `bzip2.c` and `gzip.c`) and test their functionality.

# Note on OS distributions

GCC relies on a specific glibc version or build. Gramine uses its own patched glibc, which can
cause failures without an obvious reason. This example is known to work on Ubuntu 18.04 and 20.04
and to fail on RHEL 8 and CentOS 8. There is no fix for RHEL and CentOS.

For details, see https://github.com/gramineproject/graphene/issues/2489.

# Running under each backend

Build and run in this directory, with the install of the backend active (`Documentation/quickstart/`). Pass or fail status: matrix in [`CI-Examples/README.md`](../README.md).

- **direct:** `make`, then `make check`.
- **vm:** `make check GRAMINE=gramine-vm`. Not verified.
- **sev:** `make check GRAMINE=gramine-sev`. Not verified.
- **vbs:** no recipe.
