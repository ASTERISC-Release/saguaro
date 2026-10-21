# MongoDB

Runs the MongoDB 7.0 server (`mongod`) on port 27017; a Python client inserts and fetches documents.

## Prerequisites

```sh
./install-prereqs.sh    # pymongo and libraries; downloads the MongoDB 7.0 tarball (checksum verified) into ~/.local/mongodb and links mongod into ~/.local/bin
```

## Run

Install the backend first: [`Documentation/quickstart/`](../../Documentation/quickstart/README.md).

```sh
./exec.sh --direct      # builds, runs and checks; also: --vm, --sev, --sev-snp, --sgx
```

By hand, the server in one terminal and the client in another:

```sh
make                    # make SGX=1 for SGX
gramine-direct mongod   # VM and SEV: GRAMINE_VM_INBOUND=27017:27017 gramine-vm mongod (or gramine-sev)
mongosh scripts/insert.js && mongosh scripts/fetch.js       # needs mongosh; make check uses pymongo instead
```

## Notes

- The server options are hardcoded in the manifest. `make check` prints `[ Success 1/1 ]`.
- On aarch64, MongoDB 7.0 needs an ARMv8.2-A CPU.
