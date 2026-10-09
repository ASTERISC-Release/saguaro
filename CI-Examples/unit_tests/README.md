# Unit-test programs

Small, self-contained programs for exercising specific Gramine behavior, as opposed to the
application integrations in the parent directory. Each directory follows the usual example layout
(`Makefile`, manifest template, sources); build with `make` and run with `gramine-direct`.

| directory | what it checks | how it is run |
|---|---|---|
| `multi-thread/` | per-thread TLS (`thread-tls`), pthread rwlocks (`thread-rwlock`), and a many-thread stress test (`thread-stress`) | `scripts/test.sh multithread` |
| `networks/` | a UDP socket self-test over loopback (server thread plus client: small, medium and large datagrams, ordering, a 400-packet burst); prints `UDP SOCKET TEST PASSED` | `scripts/test.sh networks` |
| `mremap/` | `mremap` growing and shrinking a mapping in place (data must survive, the address must not move); prints `ALL IN-PLACE TESTS PASSED` | `make && gramine-direct helloworld` |
| `align_test/` | an allocation / alignment probe (not a pass/fail test: it prints pointers and then waits) | `make && gramine-direct align_test` |
| `host_to_guest/` | the Tirith VM hook: a special `mmap` size handled in `libos_mmap.c` and by QEMU. Needs the VM PAL (it faults under the direct PAL by design) | VM PAL only |

`networks/Makefile` links the Tirith graphics stack (`sharedgl`, EGL/GL/X11) when
`../../../../sharedgl/build` exists next to this tree, and otherwise builds the plain-sockets
variant.
