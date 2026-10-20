# Memcached

This directory contains the Makefile and the template manifest for the most
recent version of Memcached as of this writing (v1.6.21).

# Prerequisites

Install the `libevent-dev` package. To benchmark with memcslap, also install `libmemcached-tools`.

# Quick Start

```sh
# build Memcached and the final manifest
make SGX=1

# run original Memcached against a benchmark (memtier_benchmark,
# install the benchmark on your host OS first)
./memcached &
memtier_benchmark --port=11211 --protocol=memcache_binary --hide-histogram
kill %%

# run Memcached in non-SGX Gramine against a benchmark
gramine-direct memcached &
memtier_benchmark --port=11211 --protocol=memcache_binary --hide-histogram
kill %%

# run Memcached in Gramine-SGX against a benchmark
gramine-sgx memcached &
memtier_benchmark --port=11211 --protocol=memcache_binary --hide-histogram
kill %%
```

# Memcached in a Windows VBS enclave

In WSL on an ARM64 Windows machine with VBS (`Documentation/vbs.md`, "Makefile flow"):

```sh
make VBS=1          # libevent 2.1.12 (static) and memcached built with the vbs gcc, packed, signed
make VBS=1 check    # start it in an enclave and check it from Windows (vbs-check.sh)
../../scripts/vbs/gramine-vbs memcached &
../../scripts/vbs/gramine-vbs net bench memcached-get 11211 -c 8 -n 100000
kill %%
```

libevent is not in the vbs sysroot, so `make VBS=1` downloads libevent 2.1.12-stable from its
GitHub release (SHA-256 checked) and links it statically. memcached runs with its defaults (4
worker threads, 64 MiB cache) on 127.0.0.1:11211, TCP only (`-U 0`), as uid 1000 (as in the SGX
manifest); clients run on Windows.

# Running under each backend

Build first (`make`, or `make VBS=1` for vbs) and activate the install of the backend (`Documentation/quickstart/`). Results and dates are in the matrix of `CI-Examples/README.md`.

- **direct:** `gramine-direct memcached`. Verified on x86_64 and aarch64.
- **vm:** `GRAMINE_RUNNER=gramine-vm test/direct/test-direct.sh memcached`, from the repository root (the VM forwards the server's port through its relay). Verified on x86_64 and aarch64.
- **sev:** not verified.¹
- **tdx:** not verified.²
- **sgx:** not verified.³ The commands are given above.
- **vbs:** `make VBS=1 check`. Verified on ARM64 and x64.

¹ No result is recorded for this backend (matrix in `CI-Examples/README.md`).  
² TDX was tested offline by the maintainer, with the same result as SEV. No TDX log is in the repository, so the result is reported and not verified here. The TDX install needs `td-shim` (`Documentation/TODO.md`).  
³ SGX has not run on this tree's hardware. The SGX build needs `protobuf-c` (`Documentation/TODO.md`).
