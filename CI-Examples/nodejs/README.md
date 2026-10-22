# Node.js

Runs the system Node.js (not built from source) on `helloworld.js`.

## Prerequisites

```sh
./install-prereqs.sh    # installs nodejs
```

## Run

Install the backend first: [`Documentation/quickstart/`](../../Documentation/quickstart/README.md).

```sh
./exec.sh --direct      # builds, runs and checks; also: --vm, --sev, --sev-snp, --sgx, --vbs
```

By hand:

```sh
make                    # make SGX=1 for SGX; make VBS=1 in WSL for VBS
make check              # runs helloworld.js, prints [ Success 1/1 ]; GRAMINE=saguaro-vm etc., SGX=1 or VBS=1 for those backends
```

## Notes

- The VBS recipe packs the linux-arm64 `node`, so it applies to ARM64 only.
