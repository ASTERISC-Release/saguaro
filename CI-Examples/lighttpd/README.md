# Lighttpd Example

This directory contains an example for running lighttpd in Gramine, including
the Makefile and a template for generating the manifest.

# Building lighttpd source

This example builds lighttpd from source and does not use an existing binary. To build lighttpd on
Ubuntu 20.04, install the following packages:

    sudo apt-get install -y build-essential libssl-dev zlib1g-dev

## Linux

Run `make` (non-debug) or `make DEBUG=1` (debug) in the directory.

## SGX

Run `make SGX=1` (non-debug) or `make SGX=1 DEBUG=1` (debug) in the directory to
prepare lighttpd to run on SGX.

# Running lighttpd

Execute one of the following commands to start lighttpd natively (without Gramine), on Gramine or
on Gramine-SGX, respectively.

    make start-native-server
    make start-gramine-server
    SGX=1 make start-gramine-server

These commands start the lighttpd server in the foreground. Run the client in another console.

Once the server has started, test it with `wget` or `curl`:

    wget http://127.0.0.1:8003/random/10K.1.html
    curl --compressed http://127.0.0.1:8003/random/10K.1.html -o 10K.1.html

The benchmark script uses `wrk` (wrk2) and can be run as well. See https://github.com/giltene/wrk2.

    ../common_tools/benchmark-http.sh http://127.0.0.1:8003

Use Ctrl-C to terminate the server once you are finished testing lighttpd.

# lighttpd in a Windows VBS enclave

In WSL on an ARM64 Windows machine with VBS (`Documentation/vbs.md`, "Makefile flow"):

```sh
make VBS=1          # lighttpd built with the vbs gcc, installed into vbs-build/root, packed, signed
make VBS=1 check    # start it in an enclave; fetch pages with Windows' curl.exe (vbs-check.sh)
```

lighttpd runs in the foreground (`-D`) as one process on 127.0.0.1:8003 with
`lighttpd-vbs.conf.template` (no mod_deflate: the vbs build has no zlib, OpenSSL, PCRE or bzip2).
Its modules are loaded with `dlopen` from `/lighttpd/lib`; they are packed into the signed image
like the binary.

# Running under each backend

Build first (`make`, or `make VBS=1` for vbs) and activate the install of the backend (`Documentation/quickstart/`). Results and dates are in the matrix of `CI-Examples/README.md`.

- **direct:** `make start-gramine-server`. Verified on x86_64 and aarch64.
- **vm:** `GRAMINE_RUNNER=gramine-vm test/test-direct.sh lighttpd`, from the repository root (the VM forwards the server's port through its relay). Verified on x86_64 and aarch64.
- **sev:** not verified.¹
- **tdx:** not verified.²
- **sgx:** not verified.³ The commands are given above.
- **vbs:** `make VBS=1 check`. Verified on ARM64 and x64.

¹ No result is recorded for this backend (matrix in `CI-Examples/README.md`).  
² TDX was tested offline by the maintainer, with the same result as SEV. No TDX log is in the repository, so the result is reported and not verified here. The TDX install needs `td-shim` (`Documentation/TODO.md`).  
³ SGX has not run on this tree's hardware. The SGX build needs `protobuf-c` (`Documentation/TODO.md`).
