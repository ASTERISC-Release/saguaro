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

- **VM:** the same commands as direct, with the ports forwarded through the relay:
  `make start-gramine-server GRAMINE=gramine-vm GRAMINE_VM_INBOUND=8002:8002,8444:8444`. Nginx
  starts its worker processes: the VM PAL enables multi-process support by default
  (`sys.experimental__multiprocess`, default `true`; the manifest does not set it). From the
  repository root, `GRAMINE_RUNNER=gramine-vm test/test-direct.sh nginx` runs the same.
- **SEV:** not verified. The same with `GRAMINE=gramine-sev`.
- **SGX:** not verified. `make SGX=1 start-gramine-server`.
- **VBS:** `make VBS=1 check` starts Nginx in an enclave and fetches the pages from Windows
  (HTTP only, on 8002; no SSL).
