# RA-TLS with Nginx

**NOTE:** This example requires SGX. It does not work with `gramine-direct`.

This example demonstrates how to attest a generic TLS server using RA-TLS. The
certificate and key files are saved to a tmpfs system inside Gramine using
`gramine-ratls` tool. Nginx is configured to use those files. This works because
while tmpfs is not shared between Gramine processes, Nginx is execve()'d from
`gramine-ratls`, so the tmpfs is kept intact.

```sh
make
gramine-sgx nginx
```

Then in another terminal:
```sh
curl --insecure https://localhost:8000
```

In this simplified example, `--insecure` argument to curl means that curl will
not check the TLS certificate. The `curl` command-line tool can't check it at
all, because RA-TLS certificate is self-signed and does not chain to any WebPKI
roots, so `curl` is currently unsuitable for production deployments. In
production, another HTTPS client performing remote attestation should be used.

See also `run.sh` script, which starts the enclave, performs a request, then
kills it and reports the status.

## EPID remote attestation

If you use EPID attestation, add those arguments to `make` invocation above:

```sh
make RA_TYPE=epid RA_CLIENT_SPID=... RA_CLIENT_LINKABLE=[0|1]
```

## Running under each backend

Build first (`make`, or `make VBS=1` for vbs) and activate the install of the backend (`Documentation/quickstart/`). Results and dates are in the matrix of `CI-Examples/README.md`.

- **direct:** not applicable. The example needs SGX.
- **vm:** not applicable. The example needs SGX.
- **sev:** not verified.¹
- **tdx:** not verified.²
- **sgx:** not verified.³ The commands are given above.
- **vbs:** not applicable. The example is for SGX attestation.

¹ No result is recorded for this backend (matrix in `CI-Examples/README.md`).  
² TDX was tested offline by the maintainer, with the same result as SEV. No TDX log is in the repository, so the result is reported and not verified here. The TDX install needs `td-shim` (`Documentation/TODO.md`).  
³ SGX has not run on this tree's hardware. The SGX build needs `protobuf-c` (`Documentation/TODO.md`).
