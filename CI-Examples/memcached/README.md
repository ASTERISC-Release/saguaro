# Memcached

This directory contains the Makefile and the template manifest for Memcached 1.6.21. Pass or fail
status per backend: matrix in [`CI-Examples/README.md`](../README.md).

# Building

Install the backend first (`Documentation/quickstart/`) and run all commands in this directory.
Install the `libevent-dev` package. To benchmark with `memcslap`, also install
`libmemcached-tools`.

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

The build downloads libevent 2.1.12 (SHA-256 checked) and links it statically.

# Testing

Start the server in one terminal. It listens on port 11211:

```sh
make start-gramine-server                                                       # direct
make start-gramine-server GRAMINE=gramine-vm GRAMINE_VM_INBOUND=11211:11211     # VM
make start-gramine-server GRAMINE=gramine-sev GRAMINE_VM_INBOUND=11211:11211    # SEV
make SGX=1 start-gramine-server                                                 # SGX; not verified
```

- **direct:** `gramine-direct memcached`. Verified on x86_64 and aarch64.
- **vm:** `GRAMINE_RUNNER=gramine-vm test/direct/test-direct.sh memcached`, from the repository root (the VM forwards the server's port through its relay). Verified on x86_64 and aarch64.
- **sev:** not verified.
- **tdx:** not verified.
- **sgx:** not verified. The commands are given above.
- **vbs:** `make VBS=1 check`. Verified on ARM64 and x64.

Under the VM and SEV, `GRAMINE_VM_INBOUND` forwards the port through the relay. No other setting is
needed. `GRAMINE_RUNNER=gramine-vm test/test-direct.sh memcached`, from the repository root, runs
the VM case.

Run a benchmark from another terminal (install `memtier_benchmark` on the host first):

```sh
memtier_benchmark --port=11211 --protocol=memcache_binary --hide-histogram
```

Stop the server with Ctrl-C. `make start-native-server` runs Memcached without Gramine.

For VBS, `make VBS=1 check` starts Memcached in an enclave and checks it from Windows. It runs with
its defaults on `127.0.0.1:11211`, TCP only.
