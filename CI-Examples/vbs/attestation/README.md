# Attestation (VBS enclave)

The enclave writes an attestation report for given user data; the check verifies it for that data and rejects it for other data. VBS only.

## Run

In WSL on a Windows machine with VBS enabled. Install the backend first: [`Documentation/quickstart/vbs-windows.md`](../../../Documentation/quickstart/vbs-windows.md) (machine setup: [`vbs-enable.md`](../../../Documentation/quickstart/vbs-enable.md)).

```sh
make VBS=1              # build and sign
make VBS=1 check        # run and check
saguaro-vbs attest <64 hex digits>      # by hand
```

`../exec.sh --vbs` builds, runs and checks all the VBS examples; `make` without `VBS=1` says that the example does not run elsewhere.
