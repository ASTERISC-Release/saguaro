# Busybox

Runs BusyBox 1.32 (built from source by the Makefile): a shell and its applets.

## Run

Install the backend first: [`Documentation/quickstart/`](../../Documentation/quickstart/README.md).

```sh
./exec.sh --direct      # builds, runs and checks; also: --vm, --sev, --sev-snp, --sgx, --vbs
```

By hand:

```sh
make                    # make SGX=1 for SGX; make VBS=1 in WSL for VBS
saguaro-direct busybox sh       # a shell: type ls
make check              # runs busybox echo Hello; GRAMINE=saguaro-vm etc., SGX=1 or VBS=1 for those backends
```

## Notes

- For the `/dev/attestation/` files under SGX, build with `make SGX=1 RA_TYPE=dcap` (DCAP set up first) or `RA_TYPE=epid RA_CLIENT_SPID=<spid> RA_CLIENT_LINKABLE=<0|1>`.
- Under VBS `make check` runs single applets (an enclave has one process: no shell pipelines, no `httpd`).
