# TLS echo server with attestation (VBS enclave)

An echo server that does TLS itself (VBS does not encrypt network traffic) with a certificate bound to the enclave's attestation report, and a Windows client that checks the report before sending data. VBS only.

## Run

In WSL on a Windows machine with VBS enabled. Install the backend first: [`Documentation/quickstart/vbs-windows.md`](../../../Documentation/quickstart/vbs-windows.md) (machine setup: [`vbs-enable.md`](../../../Documentation/quickstart/vbs-enable.md)).

```sh
make                    # build and sign (here make means make VBS=1); also tls-echo-debug.vbs with make VBS_DEBUG=1
make check              # runs the server and the Windows client, including the negative cases
```

By hand:

```sh
../../../scripts/vbs/saguaro-vbs tls-echo --port 4433 &
../../../scripts/vbs/saguaro-vbs tls-client 4433 --verifier tls-echo.vbs \
    --family-id 6772616d696e652d7662732d746c7300 --image-id 746c732d6563686f0000000000000001 \
    --min-svn 1 --message hello
```

`../exec.sh --vbs` builds, runs and checks all the VBS examples.

## Notes

- The server listens on `127.0.0.1:4433` (TLS) and `4434` (plain) and needs at least `--port`: the manifest passes the host's arguments.
- The report proves the key was made by a VBS enclave on this machine; a client on another machine would need Microsoft Azure Attestation, which is not implemented. The server code is `tls-echo.c` with `../common/vbs-ratls.c`.
