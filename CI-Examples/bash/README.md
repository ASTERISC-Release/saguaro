# Bash example

This directory contains an example for running Bash in Gramine, including
the Makefile and a template for generating the manifest.

# Generating the manifest

```
# Direct/VM/TDX/SEV
make

# SGX
make SGX=1
```

# Running Bash with Gramine

Examples of running Bash scripts under Gramine:

```
gramine-[direct,vm,sgx,tdx,sev,vbs] ./bash -c "ls"
gramine-[direct,vm,sgx,tdx,sev,vbs] ./bash -c "cd scripts && bash bash_test.sh 2"
```

# Support Status

Build first and activate the install of the backend (`Documentation/quickstart/`). Results and dates
are in the matrix of `CI-Examples/README.md`.

- **direct:** verified on x86_64 and aarch64.
- **vm:** verified on x86_64 and aarch64. On x86_64 the example's 7 script checks pass (7/7) with `sys.experimental__multiprocess`, which `manifest.template` sets.
- **sev:** not verified.¹
- **tdx:** not verified.²
- **sgx:** not verified.³ The commands are given above.
- **vbs:** not verified.⁴

¹ No result is recorded for this backend (matrix in `CI-Examples/README.md`).  
² TDX was tested offline by the maintainer, with the same result as SEV. No TDX log is in the repository. The TDX install needs `td-shim` (`Documentation/TODO.md`).  
³ SGX has not run on this tree's hardware. The SGX build needs `protobuf-c` (`Documentation/TODO.md`).  
⁴ No VBS recipe.
