# Redis

Runs the Redis 6.0.5 server (built from source by the Makefile) on port 6379.

## Run

Install the backend first: [`Documentation/quickstart/`](../../Documentation/quickstart/README.md).

```sh
./exec.sh --direct      # builds, runs and checks; also: --vm, --sev, --sev-snp, --sgx, --vbs
```

By hand, the server in one terminal and the client in another:

```sh
make                    # make SGX=1 for SGX; make VBS=1 in WSL for VBS
make start-saguaro-server       # VM and SEV: add SAGUARO=saguaro-vm (or saguaro-sev) SAGUARO_VM_INBOUND=6379:6379
src/src/redis-benchmark
```

`make check` starts the server, sends commands and stops it (`make SGX=1`, `make VBS=1` for those backends).

## Notes

- The manifest starts Redis with `--save ''`: saving to disk uses `fork`, which is slow in Saguaro.
- `make USE_SELECT=1` tests `select` instead of `epoll`.
- Under VBS there are no RDB snapshots or append-only file, and Redis listens on `127.0.0.1:6379` only.
