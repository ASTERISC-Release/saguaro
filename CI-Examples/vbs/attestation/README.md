# Attestation (VBS enclave)

The enclave writes an attestation report for given user data. The verifier accepts the report for
that data and rejects it for other data (`dev docs: vbs-enclave/manual.md`, "Attestation"). The example is for
the VBS backend only. Pass or fail status: matrix in [`CI-Examples/README.md`](../../README.md).

# Building

Install the VBS backend first (`dev docs: vbs-enclave/manual.md`) and run all commands in this directory.

## VBS

```sh
make VBS=1
```

The example does not run under direct, VM or SEV; `make` without `VBS=1` says so.

# Testing

```sh
make VBS=1 check
```

The check runs the enclave with 64 hex digits of user data, verifies the report for that data and
prints `[ Success ]` if the verifier also rejects the report for other data. To run the enclave by
hand:

```sh
gramine-vbs attest <64 hex digits>
```
