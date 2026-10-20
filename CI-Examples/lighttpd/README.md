# Lighttpd

This directory contains an example for running lighttpd in Gramine, including the Makefile and a
template for generating the manifest. Pass or fail status per backend: matrix in
[`CI-Examples/README.md`](../README.md).

# Building

Install the backend first (`Documentation/quickstart/`) and run all commands in this directory.
The example builds lighttpd from source. On Ubuntu, install `build-essential`, `libssl-dev` and
`zlib1g-dev`.

## Direct, VM, and SEV

```sh
make
```

## SGX

```sh
make SGX=1
```

## VBS

In WSL on a Windows machine with VBS (`Documentation/vbs.md`, "Makefile flow"):

```sh
make VBS=1
```

# Testing

- **direct:** `make start-gramine-server`. Verified on x86_64 and aarch64.
- **vm:** `GRAMINE_RUNNER=gramine-vm test/direct/test-direct.sh lighttpd`, from the repository root (the VM forwards the server's port through its relay). Verified on x86_64 and aarch64.
- **sev:** not verified.
- **tdx:** not verified.
- **sgx:** not verified. The commands are given above.
- **vbs:** `make VBS=1 check`. Verified on ARM64 and x64.

Start the server in one terminal. It listens on `127.0.0.1:8003`:

```sh
make start-gramine-server                                                     # direct
make start-gramine-server GRAMINE=gramine-vm GRAMINE_VM_INBOUND=8003:8003     # VM
make start-gramine-server GRAMINE=gramine-sev GRAMINE_VM_INBOUND=8003:8003    # SEV
make SGX=1 start-gramine-server                                               # SGX; not verified
```

Under the VM and SEV, the guest has no network of its own: `GRAMINE_VM_INBOUND` forwards the port
through the relay. No other setting is needed. `GRAMINE_RUNNER=gramine-vm test/test-direct.sh
lighttpd`, from the repository root, runs the VM case.

Fetch a test page from another terminal. The page is 10 KiB of random data, so the expected output
is the byte count:

```sh
curl -s http://127.0.0.1:8003/random/10K.1.html | wc -c    # expected: 10240
```

Stop the server with Ctrl-C. `../common_tools/benchmark-http.sh http://127.0.0.1:8003` runs a
benchmark (needs wrk2).

For VBS, `make VBS=1 check` starts lighttpd in an enclave and fetches pages from Windows. The VBS
build has no zlib, OpenSSL, PCRE or bzip2, so there is no `mod_deflate`.
