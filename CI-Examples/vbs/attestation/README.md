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

VBS only: `make VBS=1` builds, `make VBS=1 check` runs it and checks the output. The other backends do not apply (`make` without `VBS=1` says so). Status: matrix in [`CI-Examples/README.md`](../../README.md).
