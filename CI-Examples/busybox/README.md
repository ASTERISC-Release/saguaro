# Busybox

This directory contains the Makefile and the template manifest for the most recent version of
Busybox (as of this writing, commit ac78f2ac96). Pass or fail status per backend: matrix in
[`CI-Examples/README.md`](../README.md).

# Building

Install the backend first (`Documentation/quickstart/`) and run all commands in this directory.

The example builds and runs on Ubuntu 24.04 (`dev docs: aarch64/direct/TESTING.md`,
2026-10-08). With recent kernel headers, Busybox needs `CONFIG_TC`, `CONFIG_ARP` and
`CONFIG_ARPING` disabled in its `.config` (upstream bug:
https://github.com/gramineproject/gramine/issues/1909). CentOS Stream 9: not verified.

## Direct, VM, and SEV

```sh
make
```

## SGX

```sh
make SGX=1
```

To use the [`/dev/attestation/` files][attestation], build with SGX remote attestation. By default
the example is built without it. For DCAP attestation, set up DCAP first, then:

```sh
make SGX=1 RA_TYPE=dcap
```

For EPID attestation, give an SPID and say whether it is set up for linkable quotes. The dummy
values below suffice for simple experiments; real quotes need an [SPID recognized by Intel][spid]:

```sh
make SGX=1 RA_TYPE=epid RA_CLIENT_SPID=12345678901234567890123456789012 RA_CLIENT_LINKABLE=0
```

[attestation]: https://gramine.readthedocs.io/en/stable/attestation.html
[spid]: https://gramine.readthedocs.io/en/stable/sgx-intro.html#term-spid

## VBS

In WSL on a Windows machine with VBS (`dev docs: vbs-enclave/manual.md`, "Makefile flow"):

```sh
make VBS=1
```

# Testing

Run a shell, then type a command such as `ls`, which lists the current directory:

```sh
gramine-direct busybox sh
```

`make check` runs `busybox echo Hello` and prints `[ Success 1/1 ]`. It takes the backend from
`GRAMINE`:

```sh
make check                          # direct
make check GRAMINE=gramine-vm       # VM
make check GRAMINE=gramine-sev      # SEV
make SGX=1 check                    # SGX; not verified
make VBS=1 check                    # VBS
```

No backend needs another setting.
