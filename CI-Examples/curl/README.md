# Curl

This directory contains a Makefile and a template manifest to run curl on Gramine. The example uses
the curl installed on the system and does not compile it from source.

# Quick Start

To run the regression test, execute `make check`. For SGX, execute `make SGX=1 check`. The test
downloads the index page of `example.com` and therefore needs an Internet connection.

# Running under each backend

Run the commands in this directory after the build (`make`, or `make VBS=1`), with the install of the backend active (`Documentation/quickstart/`). The matrix in `CI-Examples/README.md` gives the result and the date for each backend. "Not verified" means that no result is recorded.

- **direct:** not verified.¹ Command: `make check`.
- **vm:** not verified.¹
- **sev:** not verified.¹
- **tdx:** not verified.²
- **sgx:** not verified.³ Upstream command: `make SGX=1 check`.
- **vbs:** `make VBS=1 check`. Verified on ARM64 and x64.

¹ Not verified: no result is recorded for this backend (matrix in `CI-Examples/README.md`).  
² TDX was tested offline by the maintainer, with the same result as SEV. No TDX log is in the repository, so the result is reported and not verified here. The TDX install needs `td-shim` (`Documentation/TODO.md`).  
³ SGX has not run on this tree's hardware. The SGX build needs `protobuf-c` (`Documentation/TODO.md`).
