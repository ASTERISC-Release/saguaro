# R example

This directory contains an example for running R in Gramine, including the
Makefile and a template for generating the manifest.

# Generating the manifest

## Building for Linux

Run `make` (non-debug) or `make DEBUG=1` (debug) in the directory.

## Building for SGX

Run `make SGX=1` (non-debug) or `make SGX=1 DEBUG=1` (debug) in the directory.

## Building with a local R installation

By default, the `make` command creates the manifest for the system R binary
(`/usr/lib/R/bin/exec/R`). If you have a local R installation, you may create
the manifest with the `R_HOME` variable set accordingly. For example:

```
make R_HOME=<install path>/lib/R SGX=1
```

# Run R with Gramine

When running R with Gramine, use the `--vanilla` or `--no-save` option.

Examples of running an R script under Gramine:

Without SGX:
```
gramine-direct ./R --slave --vanilla -f scripts/sample.r
gramine-direct ./R --slave --vanilla -f scripts/R-benchmark-25.R
```

With SGX:
```
gramine-sgx ./R --slave --vanilla -f scripts/sample.r
gramine-sgx ./R --slave --vanilla -f scripts/R-benchmark-25.R
```

# Running under each backend

Build first (`make`, or `make VBS=1` for vbs) and activate the install of the backend (`Documentation/quickstart/`). Results and dates are in the matrix of `CI-Examples/README.md`.

- **direct:** not verified.¹
- **vm:** not verified.¹
- **sev:** not verified.¹
- **tdx:** not verified.²
- **sgx:** not verified.³ The commands are given above.
- **vbs:** not verified.⁴

¹ No result is recorded for this backend (matrix in `CI-Examples/README.md`).  
² TDX was tested offline by the maintainer, with the same result as SEV. No TDX log is in the repository, so the result is reported and not verified here. The TDX install needs `td-shim` (`Documentation/TODO.md`).  
³ SGX has not run on this tree's hardware. The SGX build needs `protobuf-c` (`Documentation/TODO.md`).  
⁴ No VBS recipe.
