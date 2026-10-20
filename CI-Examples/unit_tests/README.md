# Unit-test programs

Small, self-contained programs for exercising specific Gramine behavior, as opposed to the
application integrations in the parent directory. Each directory follows the usual example layout
(`Makefile`, manifest template, sources). Pass or fail status per backend: matrix in
[`CI-Examples/README.md`](../README.md).

| directory | what it checks | how it is run |
|---|---|---|
| `multi-thread/` | per-thread TLS (`thread-tls`), pthread rwlocks (`thread-rwlock`), and a many-thread stress test (`thread-stress`) | `test/direct/test-direct.sh multithread` |
| `networks/` | a UDP socket self-test over loopback (server thread plus client: small, medium and large datagrams, ordering, a 400-packet burst); prints `UDP SOCKET TEST PASSED` | `test/direct/test-direct.sh networks` |
| `mremap/` | `mremap` growing and shrinking a mapping in place (data must survive, the address must not move); prints `ALL IN-PLACE TESTS PASSED` | `make && gramine-direct helloworld` |
| `align_test/` | an allocation / alignment probe (not a pass/fail test: it prints pointers and then waits) | `make && gramine-direct align_test` |

`./exec.sh --vm` (also `--direct` and `--sev`) builds and runs `mremap/`, the three programs of
`multi-thread/` and `networks/` under one backend, and skips `align_test/`. See
[`CI-Examples/README.md`](../README.md).

# Building

Install the backend first (`Documentation/quickstart/`) and run all commands in the directory of a
test.

## Direct, VM, and SEV

```sh
make
```

`networks/Makefile` links the Tirith graphics stack (`sharedgl`, EGL, GL, X11) when
`../../../../sharedgl/build` exists next to this tree, and otherwise builds the plain-sockets
variant.

## SGX

```sh
make SGX=1
```

The tests have no VBS recipe.

# Testing

- **direct:** the commands in the table above. Verified on aarch64 for `multi-thread`, `networks` and `mremap`, and on x86_64 for `multi-thread` and `mremap` (`networks` is not applicable there). `align_test` is not a test.
- **vm:** `gramine-vm thread-tls` in `multi-thread/`, `gramine-vm helloworld` in `mremap/`, and `GRAMINE_RUNNER=gramine-vm test/direct/test-direct.sh networks` from the repository root. Verified on x86_64 and aarch64.
- **sev:** `gramine-sev thread-tls`, in `multi-thread/`. Verified. Only `thread-tls` was run.
- **tdx:** reported by the maintainer to pass, offline, as under SEV.⁵
- **sgx:** not verified.
- **vbs:** not verified.¹

Run the program of a directory with the backend's command. The program is `thread-tls`,
`thread-rwlock` or `thread-stress` in `multi-thread/`, `helloworld` in `mremap/`, and
`align_test` in `align_test/`:

```sh
gramine-direct thread-tls            # direct, in multi-thread/
gramine-vm thread-tls                # VM
gramine-sev thread-tls               # SEV; only thread-tls was run
gramine-direct helloworld            # direct, in mremap/
gramine-vm helloworld                # VM
```

`networks/` is run from the repository root, with the test script:

```sh
test/test-direct.sh networks                           # direct
GRAMINE_RUNNER=gramine-vm test/test-direct.sh networks # VM
```

`make check` in `mremap/` runs `$(GRAMINE) helloworld` and compares its output with `Hello, world`;
`GRAMINE` is `gramine-direct` (`gramine-sgx` with `SGX=1`) and can be set on the command line.
The `check` targets of `multi-thread/` and `align_test/` run the same comparison, which does not
match their programs; use the commands above. No backend needs another setting.

¹ No VBS recipe.  
⁵ TDX run offline; no log in the repository.
