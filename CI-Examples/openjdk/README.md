# Java example

This directory contains an example for running OpenJDK in Gramine, including the Makefile and a
template for generating the manifest.

## Installing prerequisites

For generating the manifest and running the OpenJDK example, run the
following command to install the required packages (Ubuntu-specific):

    sudo apt-get install openjdk-11-jdk

## Building for gramine-direct

Run `make` (non-debug) or `make DEBUG=1` (debug) in the directory.

## Building for gramine-sgx

Run `make SGX=1` (non-debug) or `make SGX=1 DEBUG=1` (debug) in the directory.

## Run OpenJDK example with Gramine

Without SGX:

    gramine-direct java MultiThreadMain

With SGX:

    gramine-sgx java -Xmx8G MultiThreadMain

Note: If using 64G or greater enclave sizes, the JVM flag `-Xmx8G` can be omitted in gramine-sgx.

## Running under each backend

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
⁴ No VBS recipe; the JVM needs executable memory.
