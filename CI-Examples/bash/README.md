# Bash example

This directory contains an example for running Bash in Gramine, including
the Makefile and a template for generating the manifest.

# Generating the manifest

## Building for Linux

Run `make` (non-debug) or `make DEBUG=1` (debug) in the directory.

## Building for SGX

Run `make SGX=1` (non-debug) or `make SGX=1 DEBUG=1` (debug) in the directory.

# Running Bash with Gramine

Here's an example of running Bash scripts under Gramine:

Without SGX:
```
gramine-direct ./bash -c "ls"
gramine-direct ./bash -c "cd scripts && bash bash_test.sh 2"
```

With SGX:
```
gramine-sgx ./bash -c "ls"
gramine-sgx ./bash -c "cd scripts && bash bash_test.sh 2"
```

# Running under each backend

Run the commands in this directory after the build (`make`, or `make VBS=1`), with the install of the backend active (`Documentation/quickstart/`). The matrix in `CI-Examples/README.md` gives the result and the date for each backend. "Not verified" means that no result is recorded.

- **direct:** `gramine-direct ./bash -c "ls"`. Verified on x86_64 and aarch64.
- **vm:** `gramine-vm bash -c "ls"`. Verified on x86_64 and aarch64. The example's own script checks run only with `sys.experimental__multiprocess` (x86_64).
- **sev:** not verified.¹
- **tdx:** not verified.²
- **sgx:** not verified.³ Upstream command: `make SGX=1` and `gramine-sgx ./bash -c "ls"`.
- **vbs:** not verified.¹ No VBS recipe.

¹ Not verified: no result is recorded for this backend (matrix in `CI-Examples/README.md`).  
² TDX was tested offline by the maintainer, with the same result as SEV. No TDX log is in the repository, so the result is reported and not verified here. The TDX install needs `td-shim` (`Documentation/TODO.md`).  
³ SGX has not run on this tree's hardware. The SGX build needs `protobuf-c` (`Documentation/TODO.md`).
