# Unit tests

Small programs that exercise specific Gramine behaviour, as opposed to the application examples.

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
gramine-direct thread-tls       # multi-thread/; likewise thread-rwlock, thread-stress
gramine-direct helloworld       # mremap/
gramine-direct align_test       # align_test/
```

The `multi-thread` and `networks` suites also run from the repository root: `test/direct/test-direct.sh multithread` and `test/direct/test-direct.sh networks` (`GRAMINE_RUNNER=gramine-vm` for the VM).

## Notes

- `make check` in `mremap/`, `multi-thread/` and `align_test/` compares the output with `Hello, world`, which none of these programs prints, so it fails; use `exec.sh` or the commands above (the Makefiles need a fix).
- `networks/Makefile` builds a variant that links the shared GL stack when `../../../../sharedgl/build` exists, and the plain-sockets variant otherwise.
