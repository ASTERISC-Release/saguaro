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

Run the commands in this directory after the build (`make`, or `make VBS=1`), with the install of the backend active (`Documentation/quickstart/`). The matrix in `CI-Examples/README.md` gives the result and the date for each backend. "Not verified" means that no result is recorded.

- **direct:** not verified.¹ Command: `make check`.
- **vm:** not verified.¹
- **sev:** not verified.¹
- **tdx:** not verified.²
- **sgx:** not verified.³ Upstream command: `SGX=1 make check`.
- **vbs:** not verified.¹ No VBS recipe.

¹ Not verified: no result is recorded for this backend (matrix in `CI-Examples/README.md`).  
² TDX was tested offline by the maintainer, with the same result as SEV. No TDX log is in the repository, so the result is reported and not verified here. The TDX install needs `td-shim` (`Documentation/TODO.md`).  
³ SGX has not run on this tree's hardware. The SGX build needs `protobuf-c` (`Documentation/TODO.md`).
