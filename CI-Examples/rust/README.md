# Rust

Runs an HTTP server written in Rust (`hyper`) on `127.0.0.1:3000`; it also exercises Gramine's `epoll`.

## Prerequisites

```sh
./install-prereqs.sh    # cargo/rustc (1.85 if available), curl, build-essential; VBS also needs rustup stable with rust-src (WITH_RUSTUP=1 ./install-prereqs.sh)
```

## Run

Install the backend first: [`Documentation/quickstart/`](../../Documentation/quickstart/README.md).

```sh
./exec.sh --direct      # builds, runs and checks; also: --vm, --sev, --sev-snp, --sgx, --vbs
```

By hand, the server in one terminal and the client in another:

```sh
make                    # make SGX=1 for SGX; make VBS=1 in WSL for VBS
make start-gramine-server       # VM and SEV: add GRAMINE=gramine-vm (or gramine-sev) GRAMINE_VM_INBOUND=3000:3000
curl http://127.0.0.1:3000      # Hello, World
```

`make check` starts the server, queries it and stops it (`make SGX=1`, `make VBS=1` for those backends).

## Notes

- The address and port are hardcoded in `src/main.rs`.
