# RA-TLS with Nginx

Attests a generic TLS server with RA-TLS: `gramine-ratls` writes the certificate and key to a tmpfs inside Gramine and then `execve()`s Nginx, which uses them. SGX only (it does not run under direct, VM, SEV or VBS).

## Prerequisites

An SGX machine with Gramine installed for SGX ([`Documentation/quickstart/tdx-sgx.md`](../../../Documentation/quickstart/tdx-sgx.md)) and, for the default DCAP attestation, a working DCAP setup.

## Run

```sh
make                       # RA_TYPE=dcap by default
gramine-sgx nginx          # the server, on https://localhost:8000
curl --insecure https://localhost:8000
make check                 # runs run.sh: starts the enclave, makes a request, stops it
```

## Notes

- For EPID: `make RA_TYPE=epid RA_CLIENT_SPID=<spid> RA_CLIENT_LINKABLE=<0|1>`.
- `--insecure` makes curl skip the certificate check; curl cannot verify an RA-TLS certificate. Use a client that performs remote attestation.
