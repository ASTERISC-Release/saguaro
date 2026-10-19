# Bash example

This directory contains an example for running Bash in Gramine, including the Makefile and a
template for generating the manifest. Pass or fail status per backend: matrix in
[`CI-Examples/README.md`](../README.md).

# Building

Activate the install of the backend first (`Documentation/quickstart/`): the manifest names that
install's files. Run the commands in this directory.

## Direct, VM and SEV

```sh
make
```

## SGX

```sh
make SGX=1
```

The example has no VBS recipe.

# Testing

Under `gramine-direct`, `make regression` runs `ls` and the scripts in `scripts/` (7 checks) and
prints `[ Success N/7 ]` for each:

```sh
make regression
```

Other backends use the same target with `GRAMINE` set:

```sh
make regression GRAMINE=gramine-vm    # VM
make regression GRAMINE=gramine-sev   # SEV; not verified
make SGX=1 regression                 # SGX; not verified
```

Only the VM needs a setting: the checks start child processes, so `manifest.template` sets
`sys.experimental__multiprocess = true` (supported on x86_64 only). The other backends ignore the key.

To run a single command, for example `gramine-direct ./bash -c "ls"`, replace `gramine-direct` by
`gramine-vm`, `gramine-sev` or `gramine-sgx`.
