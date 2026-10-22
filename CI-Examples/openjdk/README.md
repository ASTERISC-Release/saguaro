# OpenJDK

Runs Java (`MultiThreadMain`, a multi-threaded program) under Saguaro.

## Prerequisites

```sh
./install-prereqs.sh    # installs OpenJDK 17, or 21, or 11 (the first one available in apt)
```

## Run

Install the backend first: [`Documentation/quickstart/`](../../Documentation/quickstart/README.md).

```sh
./exec.sh --direct      # builds, runs and checks; also: --vm, --sev, --sev-snp, --sgx
```

By hand:

```sh
make                    # make SGX=1 for SGX
saguaro-direct java MultiThreadMain     # or saguaro-vm, saguaro-sev; SGX: saguaro-sgx java -Xmx8G MultiThreadMain
```

## Notes

- Under SGX `-Xmx8G` is needed unless the enclave size is 64G or more (the manifest sets 16G).
