# iperf

Runs the iperf3 3.16 server (built from source by the Makefile) under Gramine, with the native client.

## Prerequisites

```sh
./install-prereqs.sh    # build-essential, wget
```

## Run

Install the backend first: [`Documentation/quickstart/`](../../Documentation/quickstart/README.md).

```sh
./exec.sh --direct      # builds, runs and checks; also: --vm, --sev, --sev-snp, --sgx, --vbs
```

By hand, the server in one terminal (port 5201) and the client in another:

```sh
make                    # make SGX=1 for SGX; make VBS=1 in WSL for VBS
gramine-direct iperf3   # VM and SEV: GRAMINE_VM_INBOUND=5201:5201 gramine-vm iperf3 (or gramine-sev)
LD_LIBRARY_PATH=./install ./install/iperf3 -c 127.0.0.1 -p 5201
```

## Notes

- `make VBS=1 check` runs the server and the client, each in its own enclave.
