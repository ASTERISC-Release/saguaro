# Redis

This directory contains the Makefile and the template manifest for the most
recent version of Redis (as of this writing, version 6.0.5).

The Makefile and the template manifest contain extensive comments. Review them to understand
Gramine and the requirements for applications that run under it. A new example that takes this
Redis example as a template should drop the comments, which only add noise (see Memcached for a
stripped-down example).


# Quick Start

```sh
# build Redis and the final manifest
make SGX=1

# run original Redis against a benchmark (redis-benchmark supplied with Redis)
./redis-server --save '' &
src/src/redis-benchmark
kill %%

# run Redis in non-SGX Gramine against a benchmark (args are hardcoded in manifest)
gramine-direct redis-server &
src/src/redis-benchmark
kill %%

# run Redis in Gramine-SGX against a benchmark (args are hardcoded in manifest)
gramine-sgx redis-server &
src/src/redis-benchmark
kill %%
```

# Why this Redis configuration?

Notice that we run Redis with the `save ''` setting. This setting disables
saving DB to disk (both RDB snapshots and AOF logs). We use this setting
because:

- saving DB to disk is a slow operation (Redis uses fork internally which is
  implemented as a slow checkpoint-and-restore in Gramine and requires creating
  a new SGX enclave);
- saved RDB snapshots and AOF logs must be encrypted and integrity-protected for
  DB confidentiality reasons, which requires marking the corresponding
  directories and files as `encrypted` in Gramine manifest; we skip it for
  simplicity.

In Gramine case, this setting is hardcoded in the manifest file, see
`loader.argv` there.

# Redis with Select

By default, Redis uses the epoll mechanism of Linux to monitor client
connections. To test Redis with select, add `USE_SELECT=1`, e.g., `make SGX=1
USE_SELECT=1`.

# Redis in a Windows VBS enclave

In WSL on an ARM64 Windows machine with VBS (`Documentation/vbs.md`, "Makefile flow"):

```sh
make VBS=1          # Redis 6.0.5 built with the vbs gcc in vbs-build/, packed, signed on Windows
make VBS=1 check    # start it in an enclave and check it from Windows (vbs-check.sh)
../../scripts/vbs/gramine-vbs redis-server &                         # run it
../../scripts/vbs/gramine-vbs net redis 6379 PING                    # a command from Windows
../../scripts/vbs/gramine-vbs net bench redis-get 6379 -c 8 -n 100000   # load from Windows
kill %%
```

The configuration (`vbs.manifest.template`): one process, so no RDB snapshots (`--save ""`) and no
append-only file (`--appendonly no`); `BGSAVE` and `BGREWRITEAOF` would fork and fail, and data
is lost when the enclave exits. Redis listens on 127.0.0.1 only (`--bind`; the enclave's sockets
are the Windows host's, and another interface would need a firewall rule) on port 6379 (`REDIS_PORT`
when the manifest is rendered). It uses the vbs glibc's malloc (`MALLOC=libc`) instead of jemalloc.
`vbs.enclave_size` is 1G (`VBS_ENCLAVE_SIZE` when rendering; at least 256M): about 650,000 keys
with 1 KiB values fit. Clients run on Windows: WSL2 in NAT mode cannot reach Windows' loopback.

# Running under each backend

Build and run in this directory, with the install of the backend active (`Documentation/quickstart/`). Pass or fail status: matrix in [`CI-Examples/README.md`](../README.md).

- **direct:** `make`, then `make start-gramine-server`; start the client in another terminal (see above).
- **vm:** `GRAMINE_RUNNER=gramine-vm test/test-direct.sh redis`, from the repository root. The script forwards the server port through the relay (`GRAMINE_VM_INBOUND`).
- **sev:** `make start-gramine-server GRAMINE=gramine-sev GRAMINE_VM_INBOUND=6379:6379`. Not verified.
- **vbs:** `make VBS=1 check` (builds, then runs).
