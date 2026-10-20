# RA-TLS with Nginx

This example demonstrates how to attest a generic TLS server using RA-TLS. The certificate and key
files are saved to a tmpfs inside Gramine with the `gramine-ratls` tool, and Nginx is configured to
use them. This works because tmpfs is not shared between Gramine processes, but Nginx is
`execve()`'d from `gramine-ratls`, so the tmpfs stays intact. Pass or fail status per backend:
matrix in [`CI-Examples/README.md`](../../README.md).

The example needs SGX. It does not run under direct, VM, SEV or VBS.

# Building

Install Gramine with SGX first (`Documentation/quickstart/`) and run all commands in this
directory.

## SGX

```sh
make
```

For EPID attestation, give an SPID and say whether it is set up for linkable quotes. The dummy SPID
below suffices for simple experiments:

```sh
make RA_TYPE=epid RA_CLIENT_SPID=12345678901234567890123456789012 RA_CLIENT_LINKABLE=0
```

# Testing

Start the server (not verified; SGX has not run on this tree's hardware):

```sh
gramine-sgx nginx
```

Then, in another terminal:

```sh
curl --insecure https://localhost:8000
```

`--insecure` makes curl skip the certificate check. curl cannot check an RA-TLS certificate, because
it is self-signed and does not chain to a WebPKI root, so curl is unsuitable for production; use an
HTTPS client that performs remote attestation. `make check` runs `run.sh`, which starts the
enclave, performs a request, kills it and reports the status.
