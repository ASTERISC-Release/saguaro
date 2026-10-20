# Nginx

This directory contains the Makefile and the template manifest for Nginx 1.22.0, built from source
(the Makefile downloads it). Pass or fail status per backend: matrix in
[`CI-Examples/README.md`](../README.md).

# Building

## Pre-requisites

Install `build-essential`, `libssl-dev` and `zlib1g-dev` (Ubuntu). 

## Direct, VM, and SEV

```sh
make
```

## SGX

```sh
make SGX=1
```

## VBS

In WSL on an ARM64 or x64 Windows machine with VBS (`Documentation/vbs.md`, "Makefile flow"):

```sh
make VBS=1
```

# Testing

Start the server under `gramine-direct` in one terminal. It listens on `127.0.0.1:8002` (HTTP) and
`8444` (HTTPS):

```sh
make start-gramine-server GRAMINE=<backend> GRAMINE_VM_INBOUND=8002:8002,8444:8444
# `backend` can be gramine-vm, gramine-sev, or gramine-tdx
```

Note that for **VBS:** `make VBS=1 check` starts Nginx in an enclave and fetches the pages from Windows (HTTP only, on 8002; no SSL).

Fetch a test page from another terminal. The page is 10 KiB of random data, so the expected output
is the byte count:

```sh
curl -s http://127.0.0.1:8002/random/10K.1.html | wc -c        # expected: 10240
curl -sk https://127.0.0.1:8444/random/10K.1.html | wc -c      # HTTPS, self-signed certificate
```

Stop the server with Ctrl-C. `../common_tools/benchmark-http.sh http://127.0.0.1:8002` runs a
benchmark (needs wrk2).

- **direct:** `gramine-direct ./nginx`. Verified on x86_64 and aarch64.
- **vm:** `GRAMINE_RUNNER=gramine-vm test/direct/test-direct.sh nginx`, from the repository root (the VM forwards the server's port through its relay; nginx runs with `master_process off`). Verified on x86_64 and aarch64.
- **sev:** not verified.¹
- **tdx:** not verified.²
- **sgx:** not verified.³ The commands are given above.
- **vbs:** `make VBS=1 check`. Verified on ARM64 and x64.

