# Curl

Runs the system `curl` (not built from source) against a local web server.

## Prerequisites

```sh
./install-prereqs.sh    # curl, python3, netcat
```

## Run

Install the backend first: [`Documentation/quickstart/`](../../Documentation/quickstart/README.md).

```sh
./exec.sh --direct      # builds, runs and checks; also: --vm, --sev, --sev-snp, --sgx, --vbs
```

By hand:

```sh
make                    # make SGX=1 for SGX; make VBS=1 in WSL for VBS
make check              # serves test-docroot on 127.0.0.1:19111, fetches it with curl, prints [ Success 1/1 ]
```

## Notes

- Under the VM and SEV the guest cannot reach the host's loopback: use `make check GRAMINE=gramine-vm HTTP_ADDR=<host address>` (`exec.sh` does this for you).
