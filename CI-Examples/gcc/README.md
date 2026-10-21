# GCC

Runs the system `gcc` (with `as`, `cc1`, `collect2`, `ld`) inside Gramine to compile three sample programs (`test_files/`).

## Prerequisites

```sh
./install-prereqs.sh    # build-essential, patch, wget
```

## Run

Install the backend first: [`Documentation/quickstart/`](../../Documentation/quickstart/README.md).

```sh
./exec.sh --direct      # builds, runs and checks; also: --vm, --sev, --sev-snp, --sgx
```

By hand:

```sh
make
make check              # compiles helloworld.c, bzip2.c and gzip.c and tests the results; GRAMINE=gramine-vm etc., SGX=1 for SGX
```
