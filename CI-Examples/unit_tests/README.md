# Unit-test programs

Small, self-contained programs for exercising specific Gramine behavior, as opposed to the
application integrations in the parent directory. Each directory follows the usual example layout
(`Makefile`, manifest template, sources); build with `make` and run with `gramine-direct`.

| directory | what it checks | how it is run |
|---|---|---|
| `multi-thread/` | per-thread TLS (`thread-tls`), pthread rwlocks (`thread-rwlock`), and a many-thread stress test (`thread-stress`) | `test/test-direct.sh multithread` |
| `networks/` | a UDP socket self-test over loopback (server thread plus client: small, medium and large datagrams, ordering, a 400-packet burst); prints `UDP SOCKET TEST PASSED` | `test/test-direct.sh networks` |
| `mremap/` | `mremap` growing and shrinking a mapping in place (data must survive, the address must not move); prints `ALL IN-PLACE TESTS PASSED` | `make && gramine-direct helloworld` |
| `align_test/` | an allocation / alignment probe (not a pass/fail test: it prints pointers and then waits) | `make && gramine-direct align_test` |

`networks/Makefile` links the Tirith graphics stack (`sharedgl`, EGL/GL/X11) when
`../../../../sharedgl/build` exists next to this tree, and otherwise builds the plain-sockets
variant.

## Running under each backend

Run the commands in this directory after the build (`make`, or `make VBS=1`), with the install of the backend active (`Documentation/quickstart/`). The matrix in `CI-Examples/README.md` gives the result and the date for each backend. "Not verified" means that no result is recorded.

- **direct:** the commands in the table above. Verified on aarch64 for `multi-thread`, `networks` and `mremap`, and on x86_64 for `multi-thread` and `mremap` (`networks` is not applicable there). `align_test` is not a test.
- **vm:** `gramine-vm thread-tls` in `multi-thread/`, `gramine-vm helloworld` in `mremap/`, and `GRAMINE_RUNNER=gramine-vm test/test-direct.sh networks` from the repository root. Verified on x86_64 and aarch64.
- **sev:** `gramine-sev thread-tls`, in `multi-thread/`. Verified. Only `thread-tls` was run.
- **tdx:** reported by the maintainer to pass, offline, as under SEV. No log is in the repository.²
- **sgx:** not verified.³
- **vbs:** not verified.¹ No VBS recipe.

¹ Not verified: no result is recorded for this backend (matrix in `CI-Examples/README.md`).  
² TDX was tested offline by the maintainer, with the same result as SEV. No TDX log is in the repository, so the result is reported and not verified here. The TDX install needs `td-shim` (`Documentation/TODO.md`).  
³ SGX has not run on this tree's hardware. The SGX build needs `protobuf-c` (`Documentation/TODO.md`).
