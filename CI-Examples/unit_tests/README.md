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

Build and run in this directory, with the install of the backend active (`Documentation/quickstart/`). Pass or fail status: matrix in [`CI-Examples/README.md`](../README.md).

- **direct:** `make`, then the command of the table (`make check` in `mremap/` runs `gramine-direct helloworld`).
- **vm:** `gramine-vm thread-tls` in `multi-thread/`; `make check GRAMINE=gramine-vm` in `mremap/`; `GRAMINE_RUNNER=gramine-vm test/test-direct.sh networks` from the repository root.
- **sev:** `gramine-sev thread-tls` in `multi-thread/`. Other tests: not verified.
- **vbs:** no recipe.
