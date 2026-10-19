# Curl

This directory contains a Makefile and a template manifest to run curl on Gramine. The example uses
the curl installed on the system and does not compile it from source. Pass or fail status per
backend: matrix in [`CI-Examples/README.md`](../README.md).

# Building

Install the backend first (`Documentation/quickstart/`) and run all commands in this directory.

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

`make check` starts a local web server (`python3 -m http.server` on `127.0.0.1:19111`), fetches
its page with curl and prints `[ Success 1/1 ]` if the page contains `Hello World`. It takes the
backend from `GRAMINE`:

```sh
make check                          # direct
make SGX=1 check                    # SGX; not verified
make VBS=1 check                    # VBS
```

`make check` does not work under the VM and SEV: the test server listens on the host's loopback
address, and in the guest `127.0.0.1` is the guest itself (`curl: (7) Failed to connect to
127.0.0.1 port 19111`). Serve the page on the address of the container instead, and give that
address to curl:

```sh
IP=$(hostname -I | awk '{print $1}')
(cd test-docroot && python3 -m http.server -b $IP 19111) &
gramine-vm ./curl http://$IP:19111/      # VM
gramine-sev ./curl http://$IP:19111/     # SEV
kill %%
```

Each curl command exits with 0 and prints the page (`Hello World!`).
