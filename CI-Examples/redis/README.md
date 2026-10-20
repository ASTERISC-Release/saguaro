# Redis

This directory contains the Makefile and the template manifest for Redis 6.0.5. Pass or fail
status per backend: matrix in [`CI-Examples/README.md`](../README.md). The Makefile and the
manifest template carry extensive comments; a new example that takes this one as a template should
drop them (see Memcached for a stripped-down example).

# Building

Install the backend first (`Documentation/quickstart/`) and run all commands in this directory.

## Direct, VM, and SEV

```sh
make
```

## SGX

```sh
make SGX=1
```

To test Redis with `select` instead of `epoll`, add `USE_SELECT=1`.

## VBS

In WSL on a Windows machine with VBS (`dev docs: vbs-enclave/manual.md`, "Makefile flow"):

```sh
make VBS=1
```

# Testing

Start the server in one terminal. It listens on port 6379:

```sh
make start-gramine-server                                                    # direct
make start-gramine-server GRAMINE=gramine-vm GRAMINE_VM_INBOUND=6379:6379     # VM
make start-gramine-server GRAMINE=gramine-sev GRAMINE_VM_INBOUND=6379:6379    # SEV
make SGX=1 start-gramine-server                                              # SGX; not verified
```

- **direct:** `gramine-direct redis-server`. Verified on x86_64 and aarch64.
- **vm:** `GRAMINE_RUNNER=gramine-vm test/direct/test-direct.sh redis`, from the repository root (the VM forwards the server's port through its relay). Verified on x86_64 and aarch64.
- **sev:** not verified.
- **tdx:** not verified.
- **sgx:** not verified. The commands are given above.
- **vbs:** `make VBS=1 check`. Verified on ARM64 and x64.

Under the VM and SEV, `GRAMINE_VM_INBOUND` forwards the port through the relay. No other setting is
needed. `GRAMINE_RUNNER=gramine-vm test/test-direct.sh redis`, from the repository root, runs the
VM case.

Run the benchmark supplied with Redis from another terminal:

```sh
src/src/redis-benchmark
```

Stop the server with Ctrl-C.

Redis runs with `--save ''`, set in the manifest (`loader.argv`). Saving to disk uses `fork`, which
is slow in Gramine (it needs a new SGX enclave), and the saved files would have to be encrypted
and integrity-protected.

For VBS, `make VBS=1 check` starts Redis in an enclave and checks it from Windows. The enclave holds
one process, so there are no RDB snapshots and no append-only file, and Redis listens on
`127.0.0.1:6379` only.
