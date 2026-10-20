# Python example

This directory contains an example for running Python 3 in Gramine, including the Makefile and a
template for generating the manifest. Pass or fail status per backend: matrix in
[`CI-Examples/README.md`](../README.md).

# Building

Install the backend first (`Documentation/quickstart/`) and run all commands in this directory.

## Pre-requisites

Install the packages for the manifest and the test scripts (Ubuntu):

```sh
sudo apt-get install libnss-mdns python3-numpy python3-scipy
```

## Direct, VM, and SEV

```sh
make
```

## SGX

```sh
make SGX=1
```

`scripts/sgx-quote.py` needs SGX remote attestation, which is off by default. For DCAP attestation,
set up DCAP first, then build with `make SGX=1 RA_TYPE=dcap`. For EPID attestation, give an SPID
and say whether it is set up for linkable quotes. The dummy values below suffice for simple
experiments; a verifiable quote needs an [SPID recognized by Intel][spid]:

```sh
make SGX=1 RA_TYPE=epid RA_CLIENT_SPID=12345678901234567890123456789012 RA_CLIENT_LINKABLE=0
```

[spid]: https://gramine.readthedocs.io/en/stable/sgx-intro.html#term-spid

## VBS

In WSL on an ARM64 or x64 Windows machine with VBS (`dev docs: vbs-enclave/manual.md`, "Makefile flow"). The
first build compiles CPython 3.12.7 for the enclave (`scripts/vbs/build-python.sh`, about 80 s,
kept in `build-vbs-python` of the checkout). That CPython lacks the modules whose libraries are not
in the vbs sysroot (zlib, OpenSSL, libffi, ...):

```sh
make VBS=1
```

# Testing

`make check` runs `run-tests.sh`: a hello-world script (1/4), an HTTP server with a client on
port 8005 (2/4), and NumPy and SciPy scripts (3/4, 4/4). It prints `[ Success N/4 ]` for each.
Single scripts run as `gramine-direct ./python scripts/helloworld.py`.

```sh
make check
```

The other backends differ as follows:

```sh
GRAMINE=gramine-vm GRAMINE_VM=1 GRAMINE_VM_INBOUND=8005:8005 ./run-tests.sh   # VM
gramine-sev python scripts/helloworld.py                                      # SEV; only this script
SGX=1 ./run-tests.sh                                                          # SGX; not verified
make VBS=1 check                                                              # VBS
```

- **VM:** `GRAMINE_VM=1` makes the script serve the HTTP page through the relay, forwarded by
  `GRAMINE_VM_INBOUND`, and run the client on the host.
- **VBS:** `make VBS=1 check` runs checks 1/4 and 2/4. On ARM64 the Makefile skips 3/4 and 4/4,
  because NumPy and SciPy are not packed.
