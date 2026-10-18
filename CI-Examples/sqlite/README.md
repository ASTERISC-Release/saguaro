# SQLite example

This directory contains an example for running SQLite in Gramine, including the
Makefile and a template for generating the manifest.

# Generating the manifest

## Installing prerequisite:

Please run the following command to install SQLite (Ubuntu-specific):

    sudo apt-get install sqlite3

## Building for Linux

Run `make` (non-debug) or `make DEBUG=1` (debug) in the directory.

## Building for SGX

Run `make SGX=1` (non-debug) or `make SGX=1 DEBUG=1` (debug) in the directory.

# Running SQLite with Gramine

Examples of running SQLite under Gramine (the command-line options are hardcoded in the manifest
file):

Without SGX:
```
gramine-direct sqlite3 < scripts/create.sql
gramine-direct sqlite3 < scripts/update.sql
gramine-direct sqlite3 < scripts/select.sql
```

With SGX:
```
gramine-sgx sqlite3 < scripts/create.sql
gramine-sgx sqlite3 < scripts/update.sql
gramine-sgx sqlite3 < scripts/select.sql
```

# Note about security of database files

In this example, SQLite stores the database files under the directory `db/`. The
files are encrypted by Gramine using the "encrypted" FS mount point (see the
manifest file). However, the key with which the `db/` files are encrypted is
hardcoded in the manifest. This renders this example deployment **insecure**.

A secure version will require to replace this hardcoded key with one of the
following options:

- `key_name = "_sgx_mrenclave"` or `key_name = "_sgx_mrsigner"` in the FS mount
  point in the manifest. This way, the database files will be sealed to the
  particular SGX platform and cannot be decrypted on other platforms.

- `key_name = "<provisioned key name>"` in the FS mount point in the manifest,
  plus using a key provisioning flow with SGX remote attestation (see e.g.
  `ra-tls-secret-prov` example). This way, the database files will be encrypted
  with the provisioned key and can be later decrypted on other platforms that
  possess the same key.

# Note about concurrency

SQLite uses POSIX record locks (`fcntl`) to guard concurrent accesses to the
database file. These locks are emulated within Gramine, and not translated to
host-level locks, even if you are mounting a file from the host.

That means it is safe to access the same database file from multiple processes,
but only within a **single Gramine instance**. In other words, a multi-process
Gramine application is OK, but multiple Gramine instances should not access
the same database file concurrently.

Note that in a production setup, the database should be either mounted as a
protected file, or from tmpfs, which would make it impossible to access from
multiple Gramine instances anyway.

# Running under each backend

Build first (`make`, or `make VBS=1` for vbs) and activate the install of the backend (`Documentation/quickstart/`). Results and dates are in the matrix of `CI-Examples/README.md`.

- **direct:** `gramine-direct sqlite3 < scripts/create.sql`. Verified on x86_64 and aarch64.
- **vm:** `gramine-vm sqlite3 < scripts/create.sql`. Verified on x86_64 and aarch64.
- **sev:** not verified.¹
- **tdx:** not verified.²
- **sgx:** not verified.³ The commands are given above.
- **vbs:** `make VBS=1 check`. Verified on ARM64 and x64.

¹ No result is recorded for this backend (matrix in `CI-Examples/README.md`).  
² TDX was tested offline by the maintainer, with the same result as SEV. No TDX log is in the repository, so the result is reported and not verified here. The TDX install needs `td-shim` (`Documentation/TODO.md`).  
³ SGX has not run on this tree's hardware. The SGX build needs `protobuf-c` (`Documentation/TODO.md`).
