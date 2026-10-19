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

Under the VM and SEV, `make regression` fails at its final `diff`: the launcher prints its console
lines (`Configuring Gramine VM ...`, `[boot] ...`, `[ VM exited with code 0 ]`) to stdout and so
into `OUTPUT`. Run the scripts by hand and compare the filtered output of the last one with
`scripts/select.txt`, as `test/vm-multiproc/vm-examples.sh` does.

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
