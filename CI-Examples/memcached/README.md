# Memcached

Runs the Memcached 1.6.21 server (built from source by the Makefile) on port 11211.

## Prerequisites

Package `libevent-dev` (under VBS the build downloads and links libevent itself). For benchmarks: `memtier_benchmark`.

## Run

Install the backend first: [`Documentation/quickstart/`](../../Documentation/quickstart/README.md).

```sh
./exec.sh --direct      # builds, runs and checks; also: --vm, --sev, --sev-snp, --sgx, --vbs
```

By hand, the server in one terminal and the client in another:

```sh
make
make start-saguaro-server       # VM and SEV: add SAGUARO=saguaro-vm (or saguaro-sev) SAGUARO_VM_INBOUND=11211:11211
memtier_benchmark --port=11211 --protocol=memcache_binary --hide-histogram
```

`make check` starts the server, talks to it and stops it (`make SGX=1`, `make VBS=1` for those backends).
`make start-native-server` runs Memcached without Saguaro.
