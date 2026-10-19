# Busybox

This directory contains the Makefile and the template manifest for the most
recent version of Busybox (as of this writing, commit ac78f2ac96).

## Note on Ubuntu 24.04 and CentOS Stream 9

The example builds and runs on Ubuntu 24.04 (`Documentation/devel/aarch64/direct/TESTING.md`,
2026-10-08). With recent kernel headers, Busybox needs `CONFIG_TC`, `CONFIG_ARP` and
`CONFIG_ARPING` disabled in its `.config`. CentOS Stream 9: not verified.

For the upstream bug report see https://github.com/gramineproject/gramine/issues/1909.

## Building with SGX remote attestation

To use the [`/dev/attestation/` files][attestation], build the example with SGX remote attestation
enabled. By default, the example is built *without* remote attestation.

[attestation]: https://gramine.readthedocs.io/en/stable/attestation.html

To build the example for DCAP attestation, first set up DCAP. Then build the example as follows:
```
make SGX=1 RA_TYPE=dcap
```

Otherwise, use EPID attestation. This needs an SPID, and a setting that tells whether the SPID is
set up for linkable quotes:
```
make SGX=1 RA_TYPE=epid RA_CLIENT_SPID=12345678901234567890123456789012 \
    RA_CLIENT_LINKABLE=0
```

The dummy values above suffice for simple experiments. Real SGX quotes need an [SPID recognized by
Intel][spid].

[spid]: https://gramine.readthedocs.io/en/stable/sgx-intro.html#term-spid

# Quick Start

```sh
# build Busybox and the final manifest
make SGX=1

# run Busybox shell in non-SGX Gramine
gramine-direct busybox sh

# run Busybox shell in Gramine-SGX
gramine-sgx busybox sh

# now a shell session should be running e.g. typing:
ls
# should run program `ls` which lists current working directory
```

# Running under each backend

Build and run in this directory, with the install of the backend active (`Documentation/quickstart/`). Pass or fail status: matrix in [`CI-Examples/README.md`](../README.md).

- **direct:** `make`, then `make check` (or `gramine-direct busybox sh`).
- **vm:** `make check GRAMINE=gramine-vm`.
- **sev:** `make check GRAMINE=gramine-sev`. Not verified.
- **vbs:** `make VBS=1 check` (builds, then runs).
