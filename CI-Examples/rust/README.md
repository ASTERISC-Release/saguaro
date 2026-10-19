# Rust example

This directory contains an example for running Rust in Gramine, including the Makefile and a
template for generating the manifest. The application is an HTTP server based on `hyper`; it also
tests Gramine's `epoll` implementation, as Rust's async runtimes used to trigger bugs in it. Pass or
fail status per backend: matrix in [`CI-Examples/README.md`](../README.md).

# Building

Install the backend first (`Documentation/quickstart/`) and run all commands in this directory.
The build needs `cargo`.

## Direct, VM, and SEV

```sh
make
```

## SGX

```sh
make SGX=1
```

## VBS

In WSL on a Windows machine with VBS (`Documentation/vbs.md`, "Makefile flow"). This needs rustup
stable with the `rust-src` component:

```sh
make VBS=1
```

# Testing

Start the server in one terminal. The bind address and port are hardcoded in `src/main.rs`
(`127.0.0.1:3000`):

```sh
make start-gramine-server                                                    # direct
make start-gramine-server GRAMINE=gramine-vm GRAMINE_VM_INBOUND=3000:3000     # VM
make start-gramine-server GRAMINE=gramine-sev GRAMINE_VM_INBOUND=3000:3000    # SEV; not verified
make SGX=1 start-gramine-server                                              # SGX; not verified
```

Under the VM and SEV, `GRAMINE_VM_INBOUND` forwards the port through the relay. No other setting is
needed. `GRAMINE_RUNNER=gramine-vm test/test-direct.sh rust`, from the repository root, runs the VM
case.

Query the server from another terminal. The expected output is `Hello, World`:

```sh
curl http://127.0.0.1:3000
```

Stop the server with Ctrl-C. `../common_tools/benchmark-http.sh http://127.0.0.1:3000` runs a
benchmark (needs wrk2).

For VBS, `make VBS=1 check` starts the server in an enclave and asks it for a page.
