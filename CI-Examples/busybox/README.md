# Busybox

This directory contains the Makefile and the template manifest for the most
recent version of Busybox (as of this writing, commit ac78f2ac96).

## Note on Ubuntu 24.04 and CentOS Stream 9

There is a known bug in Busybox: the build fails on newer Linux distributions such as Ubuntu 24.04
and CentOS Stream 9. No fix is available yet, so this example does not build or work on these
distributions.

For details see https://github.com/gramineproject/gramine/issues/1909.

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

Run the commands in this directory after the build (`make`, or `make VBS=1`), with the install of the backend active (`Documentation/quickstart/`). The matrix in `CI-Examples/README.md` gives the result and the date for each backend. "Not verified" means that no result is recorded.

- **direct:** `gramine-direct busybox sh`. Verified on x86_64 and aarch64.
- **vm:** `gramine-vm busybox echo Hello`. Verified on x86_64 and aarch64.
- **sev:** not verified.¹
- **tdx:** not verified.²
- **sgx:** not verified.³ Upstream command: `gramine-sgx busybox sh`.
- **vbs:** `make VBS=1 check`. Verified on ARM64 and x64.

¹ Not verified: no result is recorded for this backend (matrix in `CI-Examples/README.md`).  
² TDX was tested offline by the maintainer, with the same result as SEV. No TDX log is in the repository, so the result is reported and not verified here. The TDX install needs `td-shim` (`Documentation/TODO.md`).  
³ SGX has not run on this tree's hardware. The SGX build needs `protobuf-c` (`Documentation/TODO.md`).
