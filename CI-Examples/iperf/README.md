# iperf

This directory contains the Makefile and the template manifest for iperf 3.16. Pass or fail status
per backend: matrix in [`CI-Examples/README.md`](../README.md).

# Building

Install the backend first (`Documentation/quickstart/`) and run all commands in this directory.
iperf has no prerequisites besides a compiler and `wget` (`./install-prereqs.sh` installs them on
Debian or Ubuntu).

iperf is built from source because the Ubuntu 22.04 package `iperf3` 3.9 needs `TCP_CONGESTION`
and fails with `iperf3: error - unable to set TCP_CONGESTION`. The manual build omits it. Since
3.10, prebuilt packages should also work under Gramine (see the [release
notes](https://github.com/esnet/iperf/blob/3.16/RELNOTES.md#iperf-310-2021-05-26)).

## Direct, VM, and SEV

```sh
make
```

## SGX

```sh
make SGX=1
```

## VBS

In WSL on a Windows machine with VBS (`Documentation/vbs.md`, "Makefile flow"):

```sh
make VBS=1
```

# Testing

Start the server in one terminal. It listens on port 5201 (the iperf default):

```sh
gramine-direct iperf3                                                     # direct
GRAMINE_VM_INBOUND=5201:5201 gramine-vm iperf3                            # VM; not verified
GRAMINE_VM_INBOUND=5201:5201 gramine-sev iperf3                           # SEV; not verified
gramine-sgx iperf3                                                        # SGX; not verified
```

Under the VM and SEV, `GRAMINE_VM_INBOUND` forwards the port through the relay. The manifest sets
the server mode.

Measure from another terminal with the client of the same build (the native client is enough):

```sh
LD_LIBRARY_PATH=./install ./install/iperf3 -c localhost -p 5201
```

The output is the bandwidth per second and a summary at the end. Useful options: `-t #` (seconds),
`-n #[KMG]` (bytes), `-P #` (parallel streams), `-N` (no delay), `-1` (server: one client, then
exit). To compare with the native server, run `LD_LIBRARY_PATH=./install ./install/iperf3 -s`.

For VBS, `make VBS=1 check` runs the server and the client, each in its own enclave.
