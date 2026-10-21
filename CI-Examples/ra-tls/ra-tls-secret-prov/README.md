# Secret Provisioning

Servers and SGX enclave clients that use the Secret Provisioning library: the server (on a trusted machine, not in an enclave) verifies the client's RA-TLS certificate and quote and sends it secrets. SGX only; EPID (IAS) or DCAP attestation.

| Directory | Server sends | Client |
|---|---|---|
| `secret_prov_minimal/` | one hardcoded secret | gets it from `SECRET_PROVISION_SECRET_STRING` at start-up |
| `secret_prov/` | two secrets over the TLS connection | gets them through the C API |
| `secret_prov_pf/` | the key for encrypted files (from a file) | uses it to read the encrypted `input.txt` |

## Prerequisites

An SGX machine with Gramine installed for SGX ([`Documentation/quickstart/tdx-sgx.md`](../../../Documentation/quickstart/tdx-sgx.md)). EPID needs an [SPID and IAS API key](https://gramine.readthedocs.io/en/stable/sgx-intro.html#term-spid); DCAP needs a working DCAP setup and Gramine built with `-Ddcap=enabled`.

## Run

```sh
export RA_TLS_ALLOW_DEBUG_ENCLAVE_INSECURE=1 RA_TLS_ALLOW_OUTDATED_TCB_INSECURE=1 \
       RA_TLS_ALLOW_HW_CONFIG_NEEDED=1 RA_TLS_ALLOW_SW_HARDENING_NEEDED=1
make app dcap RA_TYPE=dcap       # or: make app epid RA_TYPE=epid RA_CLIENT_SPID=<spid> RA_CLIENT_LINKABLE=<0|1>
cd secret_prov_pf
./server_dcap wrap_key &         # EPID: RA_TLS_EPID_API_KEY=<key> ./server_epid wrap_key &
gramine-sgx ./client
kill %%
```

`make check_dcap` and `make check_epid` run all three pairs.

## Notes

- Run `make clean` between an EPID and a DCAP build.
- The enclaves are debug enclaves and the `RA_TLS_ALLOW_*` variables relax Intel's checks: never use them in production.
