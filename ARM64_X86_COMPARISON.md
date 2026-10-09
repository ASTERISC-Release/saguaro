# x86_64 vs aarch64 test comparison

Both columns come from `run_all_tests.sh` (direct PAL, `gramine-direct`).
aarch64 numbers are from `ARM64_MILESTONE_A.md`; x86_64 numbers are from the parity run
on 2026-10-08 in the `ubuntu-gpu-kvm` container (`LOGDIR=/root/x86-logs`).

| Suite | x86_64 | aarch64 | Notes |
|---|---|---|---|
| PAL regression | 32 passed / 0 failed / 4 skipped | 28 passed / 0 failed / 8 skipped | 36 tests on both. Skips are arch- or SGX-specific, so the split differs |
| LibOS regression | 146 passed / 0 failed / 32 skipped (re-run after pulling cc7a6ec4; was 144) | 142 passed / 0 failed / 36 skipped | 176 tests on both at parity-run time. Skips are arch- or SGX-specific, so the split differs. Two tests were added afterwards (fork_pacret, fcntl_dupfd) — both run on both arches, so a re-run gives 178 |
| busybox | Pass (`uname -m` -> `x86_64`) | Pass (`uname -m` -> `aarch64`) | Both need `CONFIG_TC`/`CONFIG_ARP`/`CONFIG_ARPING` off (kernel-headers issue, not port related) |
| memcached | Pass (counter semantics + 100 set/get round-trips) | Pass (3 cycles x 100 set/get, `incr` exact) | x86_64 needs `libevent-dev` |
| redis | Pass (PING/SET/GET + 20 round-trips) | Pass | |
| helloworld | Pass | Pass | |
| multi-thread | Pass (tls/rwlock/stress) | Pass | |
| bash | Pass (7/7 checks) | Pass (7/7 checks) | New `bash` suite in `run_all_tests.sh` |

Totals match (PAL 36, LibOS 176), so no test is unaccounted for. Both architectures have zero failures.

## Other CI-Examples

Run on x86_64 on 2026-10-08 (logs in `/root/x86-logs/ex`) and on aarch64 on 2026-10-08
(logs in `$LOGDIR`, `./run_all_tests.sh --no-build <suite>`; every example
below with a runner is now a suite in `run_all_tests.sh` so both machines can run the same
commands). `graphics` is excluded as requested.

| Example | x86_64 | aarch64 | How it was run / notes |
|---|---|---|---|
| sqlite | Pass (3/3) | Pass (3/3) | `make regression`. Needs the `sqlite3` package on the host. Suite `sqlite` |
| python | Pass (4/4: hello, HTTP server+client, numpy, scipy) | Pass (4/4) | `make check`. x86_64 used apt `python3-numpy`/`python3-scipy`; aarch64 used `pip3 install --user --break-system-packages numpy scipy` (the guest python runs as uid 0 which natively disables user site-packages, so the manifest re-adds it via `loader.env.PYTHONPATH`). Suite `python` |
| lighttpd | Pass | Pass | Built + served via its Makefile; `curl` fetched `/random/10K.1.html` byte-identically (port 8003). Suite `lighttpd` |
| nginx | Pass (HTTP and HTTPS) | Pass (HTTP and HTTPS) | No test target; same `curl` check on ports 8002 and 8444 (`-k`, self-signed cert), 10240 bytes each, byte-identical. aarch64 had no libssl-dev/libpcre-dev, built via `build-with-bundled-deps.sh` (nginx `--with-openssl` source build + standalone pcre; nginx 1.22's argument form `--with-pcre=DIR` does *not* compile the regex module — only the bare `--with-pcre` does). Suite `nginx` |
| blender | Pass | cannot run | `make check` renders the test scenes. Downloads Blender 2.82 as a prebuilt **x86_64-only** binary — nothing to run on aarch64 short of building Blender from source |
| mremap | Pass | Pass | The program prints "ALL IN-PLACE TESTS PASSED" on both. Its Makefile `check` target is broken (copy-pasted from helloworld, diffs against "Hello, world"), so `make check` fails regardless of the port |
| align_test | Not a test | n/a | Prints a pointer and then `sleep(400)`. Its `check` target is the same helloworld copy and cannot pass. It reaches the end of its allocations and asserts |
| host_to_guest | N/A | n/a | Exercises the Tirith VM hook (a special mmap size handled in `libos_mmap.c` and QEMU). It segfaults (rc 139) under the direct PAL by design, so it needs the VM PAL |
| rust | Not run | Pass | aarch64 installed rustup (cargo 1.99) since the x86_64 container's cargo 1.75 cannot parse `edition2024` crates — a newer toolchain fixes it there too. Running tokio required the LibOS fix `libos: F_DUPFD_CLOEXEC must honor the minimum-fd semantics` (tokio's runtime build probe `fcntl(epfd, F_DUPFD_CLOEXEC, epfd)` got EBADF). Hyper answers "Hello, World" HTTP 200 on port 3000. Suite `rust` |
| networks | N/A | Pass (self-test) | The example's `udp.c` itself is pure sockets: a UDP self-test (server thread + loopback client: small/med/big datagrams, ordering, 400-packet burst, "UDP SOCKET TEST PASSED"). The Makefile now drops the sharedgl/EGL link flags automatically when `../../../sharedgl/build` is absent. Suite `networks` |
| ra-tls-mbedtls, ra-tls-nginx, ra-tls-secret-prov | N/A | n/a | Need SGX remote attestation |

## Fixes found while running on x86_64
- `pal/regression/send_handle.c`: `memcmp` over-read past the `MSG` literal (failed on x86_64 depending on `.rodata` layout). It now compares against a zero-padded copy.
- `pal/regression/memory.c`: the arch-neutral helpers moved out of the arch `#if` block so the file builds on x86_64.
- `pal/include/arch/x86_64/pal_arch.h`: added a no-op `pal_tcb_singleton_snapshot_before_user_entry()` to match the aarch64 hook.

## Skip lists

Collected with `python3 -m pytest -q -rs` in `pal/regression` and `libos/test/regression`
(needs `PYTHONPATH` and `PATH` pointing at the Gramine install). Line numbers refer to
`test_pal.py` / `test_libos.py`. aarch64 column collected on this box 2026-10-08.

### PAL regression

| Reason | x86_64 | aarch64 |
|---|---|---|
| Needs SGX (`test_pal.py` lines 177, 466, 480, 550) | 4 | 4 (identical lines and reasons) |
| Other (arch-specific): `x86-specific` guards — `TC_00_Basic::test_Exception2` (39), `TC_01_Bootstrap::test_Segment` (69), `TC_21_Thread::test_000_exception` (260), `TC_22_SetSwitch::test_500_thread` (435) | 0 | 4 |
| **Total skipped** | **4** | **8** |

Exactly the predicted +4: the four x86-specific guards, nothing else.

### LibOS regression

| Reason | x86_64 | aarch64 |
|---|---|---|
| SGX only (file-check-policy, attestation, trusted/sealed files, CPUID/RDTSC emulation, MRENCLAVE, SIGSTRUCT) | 23 | 23 (same lines) |
| Needs `UBSAN=1` / `ASAN=1` (lines 49, 53, 56, 59) | 4 | 4 (identical lines and reasons) |
| Needs debug build (line 1362: `test_000_gdb_backtrace`) | 2 | 1 |
| (line 1400: `test_010_regs_x86_64` — counted in the row above on x86_64: its `ON_X86` decorator passes and the *runtime* debug-build check skips it there; on aarch64 the decorator skips it first, so it moves to "Other") | — | (1) |
| Needs `/dev/gramine_test_dev` (lines 1146, 1151, 1161) | 3 | 3 (identical lines and reasons) |
| Other (arch-specific): `test_110_basic_bootstrapping_cpp` (200), `test_093_sighandler_divbyzero` (999), `test_010_regs_x86_64` (~1400, see above), `TC_93_In_Out::test_000_in_out` (class-level guard, 1587), `asm/x86_64/test_x86_64.py` module (iret emulation) | 0 | 5 |
| **Total skipped** | **32** | **36** |

## Conclusion

Both suites have identical test counts on both arches (PAL 36, LibOS 176), and the
skip **sets** are fully reconciled: every difference is accounted for by an explicit
arch or SGX guard — there are no unexplained skips and, most importantly, **zero failures
on either architecture**. The predicted aarch64 "Other" gap was "4 more than x86_64"; the
actual count is 5, netting to the expected overall gap of 4 because
`test_010_regs_x86_64` is skipped on x86_64 too, just attributed there to the debug-build
runtime check instead of its `ON_X86` decorator.

x86_64 skip details verified internally consistent (32+4 and 144+32 totals match the
aarch64-collected test counts).
