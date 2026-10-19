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

Build and run in this directory, with the install of the backend active (`Documentation/quickstart/`). Pass or fail status: matrix in [`CI-Examples/README.md`](../README.md).

- **direct:** `make`, then `make start-gramine-server`; start the client in another terminal (see above).
- **vm:** `GRAMINE_RUNNER=gramine-vm test/test-direct.sh lighttpd`, from the repository root. The script forwards the server port through the relay (`GRAMINE_VM_INBOUND`).
- **sev:** `make start-gramine-server GRAMINE=gramine-sev GRAMINE_VM_INBOUND=8003:8003`. Not verified.
- **vbs:** `make VBS=1 check` (builds, then runs).
