# Bash example

This directory contains an example for running Bash in Gramine, including
the Makefile and a template for generating the manifest.

# Generating the manifest

## Building for Linux

Run `make` (non-debug) or `make DEBUG=1` (debug) in the directory.

## Building for SGX

Run `make SGX=1` (non-debug) or `make SGX=1 DEBUG=1` (debug) in the directory.

# Running Bash with Gramine

Examples of running Bash scripts under Gramine:

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

Build first (`make`, or `make VBS=1` for vbs) and activate the install of the backend (`Documentation/quickstart/`). Results and dates are in the matrix of `CI-Examples/README.md`.

- **direct:** `gramine-direct ./bash -c "ls"`. Verified on x86_64 and aarch64.
- **vm:** `gramine-vm bash -c "ls"`. Verified on x86_64 and aarch64. On x86_64 the example's 7 script checks pass (7/7) only with `sys.experimental__multiprocess` in the manifest.
- **sev:** not verified.¹
- **tdx:** not verified.²
- **sgx:** not verified.³ The commands are given above.
- **vbs:** not verified.⁴

¹ No result is recorded for this backend (matrix in `CI-Examples/README.md`).  
² TDX was tested offline by the maintainer, with the same result as SEV. No TDX log is in the repository, so the result is reported and not verified here. The TDX install needs `td-shim` (`Documentation/TODO.md`).  
³ SGX has not run on this tree's hardware. The SGX build needs `protobuf-c` (`Documentation/TODO.md`).  
⁴ No VBS recipe.
