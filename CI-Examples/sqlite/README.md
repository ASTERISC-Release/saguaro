# SQLite

Runs the SQLite shell: creates, updates and reads a database in `db/` with the scripts in `scripts/`.

## Prerequisites

```sh
./install-prereqs.sh    # sqlite3, unzip, build-essential
```

## Run

Install the backend first: [`Documentation/quickstart/`](../../Documentation/quickstart/README.md).

```sh
./exec.sh --direct      # builds, runs and checks; also: --vm, --sev, --sev-snp, --sgx, --vbs
```

By hand:

```sh
make                    # make SGX=1 for SGX; make VBS=1 in WSL for VBS
saguaro-direct sqlite3 < scripts/create.sql     # or saguaro-vm, saguaro-sev, saguaro-sgx
make regression         # prints [ Success N/3 ]; GRAMINE=saguaro-vm etc., SGX=1 for SGX; make VBS=1 check for VBS
```

## Notes

- The files in `db/` are encrypted by Gramine, but the key is hardcoded in the manifest: the example is **insecure**. For real use set `key_name` to `_sgx_mrenclave`/`_sgx_mrsigner` or provision the key (see `../ra-tls/ra-tls-secret-prov`).
- SQLite uses POSIX record locks, which Gramine emulates: access from several processes is safe only inside one Gramine instance.
- The command-line options are hardcoded in the manifest. Under VBS the shell is built from the SQLite amalgamation without readline and zlib.
