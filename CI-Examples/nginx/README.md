# Nginx

This directory contains the Makefile and the template manifest for Nginx 1.22.0, built from source
(the Makefile downloads it). Pass or fail status per backend: matrix in
[`CI-Examples/README.md`](../README.md).

# Building

Install `build-essential`, `libssl-dev` and `zlib1g-dev` (Ubuntu). Activate the install of the
backend first (`Documentation/quickstart/`): the manifest names that install's files. Run the
commands in this directory.

## Direct, VM and SEV

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
make start-gramine-server
```

Fetch a test page from another terminal. The page is 10 KiB of random data, so the expected output
is the byte count:

```sh
curl -s http://127.0.0.1:8002/random/10K.1.html | wc -c        # expected: 10240
curl -sk https://127.0.0.1:8444/random/10K.1.html | wc -c      # HTTPS, self-signed certificate
```

Stop the server with Ctrl-C. `../common_tools/benchmark-http.sh http://127.0.0.1:8002` runs a
benchmark (needs wrk2).

Other backends:

- **VM:** Nginx must run as one process. From the repository root, `GRAMINE_RUNNER=gramine-vm
  test/test-direct.sh nginx` adds `master_process off;` to `install/conf/nginx-gramine.conf`,
  forwards ports 8002 and 8444 (`GRAMINE_VM_INBOUND`) and starts the server. The curl commands are
  the same.
- **SEV:** not verified. The same, with `make start-gramine-server GRAMINE=gramine-sev
  GRAMINE_VM_INBOUND=8002:8002,8444:8444` after adding `master_process off;` to the configuration.
- **SGX:** not verified. `make SGX=1 start-gramine-server`.
- **VBS:** `make VBS=1 check` starts Nginx in an enclave and fetches the pages from Windows
  (HTTP only, on 8002; no SSL).
