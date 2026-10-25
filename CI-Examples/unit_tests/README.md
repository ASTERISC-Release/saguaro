# Unit tests

Small programs that exercise specific Saguaro behaviour, as opposed to the application examples.

| Directory | What it runs |
|---|---|
| `multi-thread/` | per-thread TLS (`thread-tls`), pthread rwlocks (`thread-rwlock`), a many-thread stress test (`thread-stress`) |
| `networks/` | a UDP socket self-test over loopback; prints `UDP SOCKET TEST PASSED` |
| `mremap/` | `mremap` growing and shrinking a mapping in place; prints `ALL IN-PLACE TESTS PASSED` |
| `align_test/` | an allocation and alignment probe for manual inspection (not a test; skipped by `exec.sh`) |

## Run

Install the backend first: [`Documentation/quickstart/`](../../Documentation/quickstart/README.md).

```sh
./exec.sh --direct      # builds and runs mremap, multi-thread and networks, and checks; also: --vm, --sev, --sev-snp
```

By hand, in the directory of a test:

```sh
make                    # make SGX=1 for SGX
saguaro-direct thread-tls       # multi-thread/; likewise thread-rwlock, thread-stress
saguaro-direct helloworld       # mremap/
saguaro-direct align_test       # align_test/
```

The `exec.sh --<backend>` of this folder runs all of them (the `smoke` tier of `test/quick.sh`); the deprecated suites `test/direct/test-direct.sh multithread` and `test/direct/test-direct.sh networks` (`SAGUARO_RUNNER=saguaro-vm` for the VM).

## Notes

- `make check` in `mremap/` and `multi-thread/` runs the programs and fails on a non-zero exit status or a missing `ALL IN-PLACE TESTS PASSED`; `align_test/` has no `check` (a manual probe).
- `networks/Makefile` builds a variant that links the shared GL stack when `../../../../sharedgl/build` exists, and the plain-sockets variant otherwise.
