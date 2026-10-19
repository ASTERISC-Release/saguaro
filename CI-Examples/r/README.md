# R example

This directory contains an example for running R in Gramine, including the Makefile and a template
for generating the manifest. Pass or fail status per backend: matrix in
[`CI-Examples/README.md`](../README.md).

# Building

Install the backend first (`Documentation/quickstart/`) and run all commands in this directory.
By default the manifest is made for the system R binary (`/usr/lib/R/bin/exec/R`). For a local R
installation, set `R_HOME`, for example `make R_HOME=<install path>/lib/R`.

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

`make check` runs `scripts/sample.r` and prints `[ Success 1/1 ]` if the output contains
`success`. It takes the backend from `GRAMINE`:

```sh
make check                          # direct
make check GRAMINE=gramine-vm       # VM
make check GRAMINE=gramine-sev      # SEV
make SGX=1 check                    # SGX; not verified
```

R needs the `--vanilla` or `--no-save` option under Gramine. To run a script by hand:

```sh
gramine-direct ./R --slave --vanilla -f scripts/R-benchmark-25.R
```

For another backend, replace `gramine-direct` by `gramine-vm`, `gramine-sev` or `gramine-sgx`.
