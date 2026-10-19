# TLS echo server with attestation (VBS enclave, optional example)

VBS does not encrypt network traffic: the enclave's sockets are the Windows host's, and Windows
(and anyone on the path) sees what an application sends. An application that needs confidentiality
on the network adds TLS itself. This example shows how, and how a client can know that the TLS peer
is the attested enclave. Nothing here changes Gramine or the other examples: TLS is the
application's choice, and the helper `../common/vbs-ratls.c` is compiled only by applications that
include it.

What happens:

- **In the enclave** (`tls-echo.c` with `../common/vbs-ratls.c`, mbedtls 3.6.0 built with the vbs
  gcc): at start an ECDSA P-256 key pair is generated inside the enclave (mbedtls's DRBG seeded
  from `getrandom()`, which the LibOS serves from the enclave's own generator); the VBS attestation
  report is read from `/dev/attestation/report` with user data = SHA-256 of the public key's DER
  SubjectPublicKeyInfo followed by 32 zero bytes; a self-signed certificate for the key carries
  the report in a non-critical X.509 extension, OID `2.25.51004656318469523056256505321196161405`
  (an OCTET STRING with the report package). The server then echoes on 127.0.0.1:4433 with TLS
  and, for comparisons, on 127.0.0.1:4434 without.
- **On Windows** (`scripts/vbs/win/vbs-tls-client.c`, SChannel, run with `gramine-vbs tls-client`):
  after the TLS handshake and before any application data, the client takes the report out of the
  certificate, checks the key binding (user data = SHA-256 of the certificate's public key), and
  runs `pal_vbs_host.exe <image>.dll 1 verify` on the report: the platform signature
  (`EnclaveVerifyAttestationReport` in a fresh enclave of the image), the user data, the unique ID
  and author ID of the image (`--same-image`), the pinned family ID, image ID and minimum SVN, and
  no debug enclave unless `--allow-debug`. Any failure closes the connection without sending
  anything.

Build and check (WSL on the Windows machine, as the other examples):

    make                  # mbedtls and the server for the enclave, the signed image tls-echo.vbs
    make check            # also builds tls-echo-debug.vbs (vbs.debug = true), then the tests below
    make clean

    ../../../scripts/vbs/gramine-vbs tls-echo --port 4433 &
    ../../../scripts/vbs/gramine-vbs tls-client 4433 --verifier tls-echo.vbs \
        --family-id 6772616d696e652d7662732d746c7300 --image-id 746c732d6563686f0000000000000001 \
        --min-svn 1 --message hello [-v]

The server takes at least one argument (`--port`), because its manifest passes the host's
arguments (`loader.insecure__use_cmdline_argv`, for the ports and the test options): they are
untrusted, and the client's checks do not depend on them.

`make check` runs the genuine case and these negative cases, each refused:

| Case | Refused by |
|---|---|
| another image ID pinned | the verifier: image id |
| another family ID pinned | the verifier: family id |
| `--min-svn 2` (the image has SVN 1) | the verifier: svn |
| server option `--test-wrong-key`: report bound to another key than the TLS key | the client: key binding |
| server option `--test-tamper-report`: as above, then the report's user data rewritten to match | the verifier: platform signature |
| debug enclave (`tls-echo-debug`) | the verifier: debug flag; accepted with `--allow-debug` |
| debug enclave checked against the release image | the verifier: unique id |

**What is verified, and what is not.** Verified: the report was produced by a VBS enclave on
**this machine** (the platform signature, checked through `EnclaveVerifyAttestationReport`, which
only verifies reports of the machine it runs on), with the expected code and signer (unique ID and
author ID of the image the client trusts), IDs, SVN and debug flag; and the enclave chose the TLS
key (key binding), so the TLS session ends in that enclave. Not verified: anything for a client on
another machine (that needs Microsoft Azure Attestation with the machine's TPM boot log, not
implemented); freshness of the report (it is made once at start; the binding to a key that never
leaves the enclave makes replay useless, but the client cannot tell how old the instance is); the
certificate's validity dates (the enclave's clock is the host's); revocation. The owner ID in the
report is chosen by the host and proves nothing.

Costs (2026-10-15, same machine, single runs; see `Documentation/devel/vbs-enclave/TESTING.md`):
key generation 0.3 ms, report 4 ms, certificate 0.3 ms at start; first connection: TLS 1.2
handshake 7.5 ms plus 107 ms for the verifier (it starts a verifying enclave); later handshakes
3.2 ms; echo round trips of 64 bytes 0.056 ms with TLS against 0.039 ms plain, 16 KiB 114 MB/s
against 349 MB/s, 256 KiB 135 MB/s against 754 MB/s each way.

## Running under each backend

VBS only: `make` builds, `make check` runs it with the Windows client. The other backends do not apply (`make` here means `make VBS=1`). Status: matrix in [`CI-Examples/README.md`](../../README.md).
