# Python

Runs Python 3 with four scripts: hello world, an HTTP server with a client (port 8005), NumPy and SciPy.

## Prerequisites

```sh
sudo apt-get install libnss-mdns python3-numpy python3-scipy
```

## Run

Install the backend first: [`Documentation/quickstart/`](../../Documentation/quickstart/README.md).

```sh
./exec.sh --direct      # builds, runs and checks; also: --vm, --sev, --sev-snp, --sgx, --vbs
```

By hand:

```sh
make                    # make SGX=1 for SGX; make VBS=1 in WSL for VBS
saguaro-direct ./python scripts/helloworld.py
make check              # runs run-tests.sh, prints [ Success N/4 ]; SGX=1 or VBS=1 for those backends
```

## Notes

- VM and SEV: `GRAMINE=saguaro-vm SAGUARO_VM=1 SAGUARO_VM_INBOUND=8005:8005 ./run-tests.sh` (`SAGUARO_VM=1` makes the HTTP client run on the host through the relay).
- SGX: `scripts/sgx-quote.py` needs remote attestation: `make SGX=1 RA_TYPE=dcap` (DCAP set up first) or `RA_TYPE=epid RA_CLIENT_SPID=<spid> RA_CLIENT_LINKABLE=<0|1>`.
- VBS: the first build compiles CPython 3.12.7 for the enclave (`scripts/vbs/build-python.sh`, kept in `build-vbs-python`); that build lacks zlib, OpenSSL and libffi modules. On ARM64, checks 3/4 and 4/4 (NumPy, SciPy) are skipped.
