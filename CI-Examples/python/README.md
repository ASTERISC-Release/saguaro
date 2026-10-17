# Python example

This directory contains an example for running Python 3 in Gramine, including
the Makefile and a template for generating the manifest.

# Generating the manifest

## Installing prerequisites

For generating the manifest and running the test scripts, please run the following
command to install the required packages (Ubuntu-specific):

    sudo apt-get install libnss-mdns python3-numpy python3-scipy

## Building for Linux

Run `make` (non-debug) or `make DEBUG=1` (debug) in the directory.

## Building for SGX

Run `make SGX=1` (non-debug) or `make SGX=1 DEBUG=1` (debug) in the directory.

If you want to run the `scripts/sgx-quote.py` script, you must build the example
with SGX remote attestation enabled. By default, the example is built *without*
remote attestation.

If you want to build the example for DCAP attestation, first make sure you have
a working DCAP setup. Then build the example as follows:
```
make SGX=1 RA_TYPE=dcap
```

Otherwise, you will probably want to use EPID attestation. For this, you will
additionally need to provide an SPID and specify whether it is set up for
linkable quotes or not:

```
make SGX=1 RA_TYPE=epid RA_CLIENT_SPID=12345678901234567890123456789012 \
    RA_CLIENT_LINKABLE=0
```

The above dummy values will suffice for simple experiments, but if you wish to
run `sgx-quote.py` and verify the output, you will need to provide an
[SPID recognized by Intel][spid].

[spid]: https://gramine.readthedocs.io/en/stable/sgx-intro.html#term-spid

## Building for a Windows VBS enclave

In WSL on an ARM64 Windows machine with VBS (see `Documentation/vbs.md`, "Makefile flow"), run
`make VBS=1` in the directory. The first time it builds CPython 3.12.7 for the enclave against the
vbs glibc (`scripts/vbs/build-python.sh`, about 80 s, kept in `build-vbs-python` of the checkout);
then it renders `vbs.manifest.template` into `python.manifest.vbs`, packs the interpreter, its
standard library, the vbs glibc, `scripts/` (as `/app/scripts`) and `helper-files/hosts` into
`python.vbs/`, and links and signs `python.dll` on Windows. That CPython lacks the modules whose
libraries are not in the vbs sysroot (zlib, OpenSSL, libffi, ...) and has no NumPy or SciPy, so
`make VBS=1 check` runs tests 1/4 (hello world) and 2/4 (HTTP server and client, both in
enclaves; the page is also fetched with Windows' `curl.exe`) and skips 3/4 and 4/4.

# Run Python with Gramine

Here's an example of running Python scripts under Gramine:
```
gramine-sgx ./python scripts/helloworld.py
gramine-sgx ./python scripts/test-numpy.py
gramine-sgx ./python scripts/test-scipy.py
gramine-sgx ./python scripts/sgx-report.py
gramine-sgx ./python scripts/sgx-quote.py
```

You can also manually run included tests:
```
SGX=1 ./run-tests.sh
```

To run Gramine in non-SGX (direct) mode, replace `gramine-sgx` with
`gramine-direct` and remove `SGX=1` in the commands above. In a VBS enclave
(`scripts/vbs/gramine-vbs` of the repository; the script paths are relative to `/app`):
```
gramine-vbs ./python scripts/helloworld.py
VBS=1 GRAMINE=$(realpath ../../scripts/vbs/gramine-vbs) ./run-tests.sh
```
