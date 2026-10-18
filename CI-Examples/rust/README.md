# Rust example

This directory contains an example for running Rust in Gramine, including the
Makefile and a template for generating the manifest. The example application is
an HTTP server based on `hyper`, to also serve as a test for Gramine's `epoll`
implementation, as Rust's async runtimes used to trigger bugs in it.

The bind address and port are hardcoded in `src/main.rs`.

# Quick Start

NOTE: The `benchmark-http.sh` script uses the wrk benchmark (wrk2). See https://github.com/giltene/wrk2.

```sh
# build the program and the final manifest
make SGX=1

# run the server in Gramine-SGX against an HTTP benchmark
make SGX=1 start-gramine-server &
../common_tools/benchmark-http.sh http://127.0.0.1:3000
kill -SIGINT %%
```

To test with non-SGX Gramine instead, omit `SGX=1` in both commands.

# Running under each backend

Build first (`make`, or `make VBS=1` for vbs) and activate the install of the backend (`Documentation/quickstart/`). Results and dates are in the matrix of `CI-Examples/README.md`.

- **direct:** `make start-gramine-server`. Verified on x86_64 and aarch64.
- **vm:** `GRAMINE_RUNNER=gramine-vm test/test-direct.sh rust`, from the repository root (the VM forwards the server's port through its relay). Verified on x86_64 and aarch64.
- **sev:** not verified.¹
- **tdx:** not verified.²
- **sgx:** not verified.³ The commands are given above.
- **vbs:** `make VBS=1 check`. Verified on ARM64 and x64.

¹ No result is recorded for this backend (matrix in `CI-Examples/README.md`).  
² TDX was tested offline by the maintainer, with the same result as SEV. No TDX log is in the repository, so the result is reported and not verified here. The TDX install needs `td-shim` (`Documentation/TODO.md`).  
³ SGX has not run on this tree's hardware. The SGX build needs `protobuf-c` (`Documentation/TODO.md`).
