# Introduction

***TODO: this example should be reworked to use a standalone gcc version, not
the one from the system***

This directory contains a Makefile and template manifests to run gcc and its
related tools on Gramine. This example uses the package version of gcc and
related tools (as, cc1, collect2, ld) installed on the system instead of
compiling them from source as some of the other examples do.

# Quick Start

To run the regression tests execute ```make check```. To do the same for SGX,
execute ```SGX=1 make check```. The regression tests build three sample programs
- helloworld.c, bzip2.c and gzip.c - and test their functionality.

# Note on OS distributions

It is known that GCC relies on specific Glibc version/build. Because Gramine
uses its own patched Glibc, this may lead to seemlingly inexplicable failures.
In particular, this GCC example is known to work on Ubuntu 18.04/20.04 but fail
on RHEL 8/CentOS 8. There is currently no fix for RHEL/CentOS.

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
