# Hello World

A minimal C program that prints `Hello, world`: the sanity test of a Saguaro installation.

## Run

Install the backend first: [`Documentation/quickstart/`](../../Documentation/quickstart/README.md).

```sh
./exec.sh --direct      # builds, runs and checks; also: --vm, --sev, --sev-snp, --sgx, --vbs
```

By hand:

```sh
make                    # make SGX=1 for SGX; make VBS=1 in WSL for VBS
saguaro-direct helloworld       # or saguaro-vm, saguaro-sev, saguaro-sgx, saguaro-vbs
make check              # make check SAGUARO=saguaro-vm for another backend; make VBS=1 check for VBS
```

`make DEBUG=1` builds with debug logging.
