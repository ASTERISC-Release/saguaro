# SQLite example

This directory contains an example for running SQLite in Gramine, including the Makefile and a
template for generating the manifest. Pass or fail status per backend: matrix in
[`CI-Examples/README.md`](../README.md).

# Building

Install the backend first (`Documentation/quickstart/`) and run all commands in this directory.
Install SQLite (Ubuntu): `sudo apt-get install sqlite3`.

## Direct, VM, and SEV

```sh
make
```

## SGX

```sh
make SGX=1
```

## VBS

In WSL on a Windows machine with VBS (`Documentation/vbs.md`, "Makefile flow"). The shell is built
from the SQLite amalgamation, without readline and zlib:

```sh
make VBS=1
```

# Testing

`make regression` creates, updates and reads the database in `db/` with the scripts in `scripts/`,
compares the last output with `scripts/select.txt` and prints `[ Success N/3 ]`. It takes the
backend from `GRAMINE`:

```sh
make regression                          # direct
make SGX=1 regression                    # SGX; not verified
make VBS=1 check                         # VBS
```

To run one script by hand (the command-line options are hardcoded in the manifest):

```sh
gramine-direct sqlite3 < scripts/create.sql
```

No backend needs another setting.

# Notes

The database files in `db/` are encrypted by Gramine, but the key is hardcoded in the manifest, so
the example is **insecure**. A secure version sets `key_name = "_sgx_mrenclave"` or
`"_sgx_mrsigner"` (sealed to the SGX platform), or a provisioned key (`key_name = "<provisioned key
name>"` with remote attestation; see `ra-tls-secret-prov`).

SQLite guards concurrent access with POSIX record locks (`fcntl`). Gramine emulates them and does
not translate them to host locks, so access from several processes is safe only within a single
Gramine instance. In production, mount the database as a protected file or from tmpfs.

# Running under each backend

Build first (`make`, or `make VBS=1` for vbs) and activate the install of the backend (`Documentation/quickstart/`). Results and dates are in the matrix of `CI-Examples/README.md`.

- **direct:** `gramine-direct sqlite3 < scripts/create.sql`. Verified on x86_64 and aarch64.
- **vm:** `gramine-vm sqlite3 < scripts/create.sql`. Verified on x86_64 and aarch64.
- **sev:** not verified. No result is recorded for this backend (matrix in `CI-Examples/README.md`).
- **tdx:** not verified.⁵
- **sgx:** not verified. SGX has not run on this tree's hardware. The SGX build needs `protobuf-c` (`Documentation/TODO.md`). The commands are given above.
- **vbs:** `make VBS=1 check`. Verified on ARM64 and x64.

With `GRAMINE=gramine-vm` or `GRAMINE=gramine-sev`, `make regression` ignores the launcher's console lines when it compares the output.

⁵ TDX run offline; no log in the repository.
