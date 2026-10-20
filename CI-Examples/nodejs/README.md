# Node.js

This directory contains a Makefile and a template manifest to run Node.js on Gramine. The example
uses the Node.js installed on the system and does not compile it from source.

# Quick Start

To run the regression test, execute `make check`. For SGX, execute `make SGX=1 check`.

# Running under each backend

Build first (`make`, or `make VBS=1` for vbs) and activate the install of the backend (`Documentation/quickstart/`). Results and dates are in the matrix of `CI-Examples/README.md`.

- **direct:** not verified.¹
- **vm:** not verified.¹
- **sev:** not verified.¹
- **tdx:** not verified.²
- **sgx:** not verified.³ The commands are given above.
- **vbs:** `make VBS=1` fails on ARM64 (x18 in the node binary and in libstdc++). Not run on x64: the recipe packs the linux-arm64 node.

¹ No result is recorded for this backend (matrix in `CI-Examples/README.md`).  
² TDX was tested offline by the maintainer, with the same result as SEV. No TDX log is in the repository, so the result is reported and not verified here. The TDX install needs `td-shim` (`Documentation/TODO.md`).  
³ SGX has not run on this tree's hardware. The SGX build needs `protobuf-c` (`Documentation/TODO.md`).
