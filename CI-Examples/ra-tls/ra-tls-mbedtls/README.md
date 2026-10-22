# RA-TLS with mbedTLS

A minimal TLS server and client based on mbedTLS examples. The server runs in an SGX enclave with a self-signed RA-TLS certificate that embeds its SGX quote; the client verifies the quote through an RA-TLS callback (EPID or DCAP). The client normally runs outside SGX. SGX only.

## Prerequisites

An SGX machine with Gramine installed for SGX ([`Documentation/quickstart/tdx-sgx.md`](../../../Documentation/quickstart/tdx-sgx.md)). EPID needs an [SPID and IAS API key](https://gramine.readthedocs.io/en/stable/sgx-intro.html#term-spid); DCAP needs a working DCAP setup and Gramine built with `-Ddcap=enabled`.

## Run

```sh
make clean && make app epid RA_TYPE=epid RA_CLIENT_SPID=<spid> RA_CLIENT_LINKABLE=<0|1>   # or: make app dcap RA_TYPE=dcap
saguaro-sgx ./server &
RA_TLS_MRENCLAVE=<..> RA_TLS_MRSIGNER=<..> RA_TLS_ISV_PROD_ID=<..> RA_TLS_ISV_SVN=<..> ./client dcap   # or: ./client epid
kill %%
```

Plain TLS without SGX: `make app && ./server & ./client native`. `make check_epid` and `make check_dcap` run the SGX flow end to end (`_fail` variants: a server sending a bad quote, `--test-malicious-quote`, must be rejected).

## Notes

- Get the enclave values with `saguaro-sgx-sigstruct-view server.sig`. Without `RA_TLS_*` values pass them as four arguments: `./client dcap <MRENCLAVE> <MRSIGNER> <ISV_PROD_ID> <ISV_SVN>`.
- EPID also needs `RA_TLS_EPID_API_KEY=<key>` for the client.
- The enclaves are debug enclaves, so set `RA_TLS_ALLOW_DEBUG_ENCLAVE_INSECURE=1`, and `RA_TLS_ALLOW_OUTDATED_TCB_INSECURE=1`, `RA_TLS_ALLOW_HW_CONFIG_NEEDED=1`, `RA_TLS_ALLOW_SW_HARDENING_NEEDED=1` if Intel advisories are not addressed. Never use these in production.
- The client can also run in SGX: `saguaro-sgx ./client_dcap dcap <...>` (build `make app dcap`), or `./client_epid epid` after `make app epid`.
