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

- **direct:** Verified on x86_64 and aarch64.
- **vm:** Verified on x86_64 and aarch64.
  - The example's own script checks run only with `sys.experimental__multiprocess` (x86_64).
- **sev:** not verified.
- **tdx:** not verified.
- **sgx:** not verified.¹ Upstream command: `make SGX=1` and `gramine-sgx ./bash -c "ls"`.
- **vbs:** not verified. No VBS recipe.
  
¹ SGX has not run on this tree's hardware. The SGX build needs `protobuf-c` (`Documentation/TODO.md`).
