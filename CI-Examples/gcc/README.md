# GCC

***TODO: this example should be reworked to use a standalone gcc version, not the one from the
system.***

This directory contains a Makefile and template manifests to run gcc and its related tools on
Gramine. The example uses the packaged gcc and related tools (`as`, `cc1`, `collect2`, `ld`)
installed on the system. Pass or fail status per backend: matrix in
[`CI-Examples/README.md`](../README.md).

GCC relies on a specific glibc version or build, and Gramine uses its own patched glibc. The example
is known to work on Ubuntu 18.04 and 20.04 and to fail on RHEL 8 and CentOS 8, with no fix (see
https://github.com/gramineproject/graphene/issues/2489).

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

The example has no VBS recipe.

# Testing

`make check` compiles three sample programs (`test_files/helloworld.c`, `bzip2.c`, `gzip.c`) with
the gcc under Gramine and tests the results. It takes the backend from `GRAMINE`:

```sh
make check                          # direct
make check GRAMINE=gramine-vm       # VM; not verified
make check GRAMINE=gramine-sev      # SEV; not verified
make SGX=1 check                    # SGX; not verified
```

gcc starts `cc1`, `as` and `ld` as child processes. The manifest does not set
`sys.experimental__multiprocess`, so under the VM and SEV the PAL default applies (`true`).
