# Bash example

This directory contains an example for running Bash in Gramine, including
the Makefile and a template for generating the manifest.

# Generating the manifest

```
# direct, VM, SEV
make

# SGX
make SGX=1
```

# Running Bash with SGX

```
gramine-sgx ./bash -c "ls"
```

# Running under each backend

Build and run in this directory, with the install of the backend active (`Documentation/quickstart/`). Pass or fail status: matrix in [`CI-Examples/README.md`](../README.md).

- **direct:** `make`, then `make regression`.
- **vm:** `make regression GRAMINE=gramine-vm`. `manifest.template` sets `sys.experimental__multiprocess = true`, which the 7 checks need (x86_64 only).
- **sev:** `make regression GRAMINE=gramine-sev`. Not verified.
- **vbs:** no recipe.
