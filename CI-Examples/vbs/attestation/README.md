# Attestation (VBS enclave)

The enclave writes an attestation report for given user data. The verifier accepts the report for
that data and rejects it for other data. The example is for the VBS backend only
(`Documentation/vbs.md`, "Attestation").

```sh
make VBS=1                       # build, pack and sign
gramine-vbs attest <64 hex digits>
make VBS=1 check                 # run it and verify the report
```

## Running under each backend

Build first (`make`, or `make VBS=1` for vbs) and activate the install of the backend (`Documentation/quickstart/`). Results and dates are in the matrix of `CI-Examples/README.md`.

- **direct, vm, sev, tdx, sgx:** not applicable. The example is for the VBS backend only (`make` without `VBS=1` says so).
- **vbs:** `make VBS=1 check`. Verified on ARM64 and x64.
