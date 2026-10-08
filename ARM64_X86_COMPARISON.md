# x86_64 vs aarch64 test comparison

Both columns come from `run_all_tests.sh` (direct PAL, `gramine-direct`).
aarch64 numbers are from `ARM64_MILESTONE_A.md`; x86_64 numbers are from the parity run
on 2026-10-08 in the `ubuntu-gpu-kvm` container (`LOGDIR=/root/x86-logs`).

| Suite | x86_64 | aarch64 | Notes |
|---|---|---|---|
| PAL regression | 32 passed / 0 failed / 4 skipped | 28 passed / 0 failed / 8 skipped | 36 tests on both. Skips are arch- or SGX-specific, so the split differs |
| LibOS regression | 144 passed / 0 failed / 32 skipped | 140 passed / 0 failed / 36 skipped | 176 tests on both. Arch-specific tests (e.g. musl vs glibc) skip on one side and run on the other |
| busybox | Pass (`uname -m` -> `x86_64`) | Pass (`uname -m` -> `aarch64`) | Both need `CONFIG_TC`/`CONFIG_ARP`/`CONFIG_ARPING` off (kernel-headers issue, not port related) |
| memcached | Pass (counter semantics + 100 set/get round-trips) | Pass (3 cycles x 100 set/get, `incr` exact) | x86_64 needs `libevent-dev` |
| redis | Pass (PING/SET/GET + 20 round-trips) | Pass | |
| helloworld | Pass | Pass | |
| multi-thread | Pass (tls/rwlock/stress) | Pass | |
| bash | Pass (7/7 checks) | not run | New `bash` suite in `run_all_tests.sh` |

Totals match (PAL 36, LibOS 176), so no test is unaccounted for. Both architectures have zero failures.

## Other CI-Examples (x86_64 only so far)

Run on x86_64 on 2026-10-08, logs in `/root/x86-logs/ex`. aarch64 is not run for any of
these yet; fill that column when you run them on aarch64. `graphics` is excluded as requested.

| Example | x86_64 | aarch64 | How it was run / notes |
|---|---|---|---|
| sqlite | Pass (3/3) | not run | `make regression`. Needs the `sqlite3` package on the host |
| python | Pass (4/4: hello, HTTP server+client, numpy, scipy) | not run | `make check`. Needs `python3-numpy`, `python3-scipy`, `wget` on the host |
| lighttpd | Pass | not run | No test target. Started under `gramine-direct`, `curl` fetched `/random/10K.1.html` (10240 bytes) |
| nginx | Pass (HTTP and HTTPS) | not run | No test target. Same `curl` check on ports 8002 and 8444 (`-k`, self-signed cert), 10240 bytes each |
| blender | Pass | not run | `make check` renders the test scenes (about 30 s for the one logged). Downloads Blender 2.82 (x86_64 build), so it will not run on aarch64 as is |
| mremap | Pass | not run | The program prints "ALL IN-PLACE TESTS PASSED". Its Makefile `check` target is broken (copy-pasted from helloworld, diffs against "Hello, world"), so `make check` fails regardless of the port |
| align_test | Not a test | n/a | Prints a pointer and then `sleep(400)`. Its `check` target is the same helloworld copy and cannot pass. It reaches the end of its allocations and asserts |
| host_to_guest | N/A | n/a | Exercises the Tirith VM hook (a special mmap size handled in `libos_mmap.c` and QEMU). It segfaults (rc 139) under the direct PAL by design, so it needs the VM PAL |
| rust | Not run | not run | Build fails: the container's cargo 1.75 cannot parse current crates (`edition2024`). Needs a newer toolchain |
| networks | N/A | n/a | Tirith graphics plumbing (sharedgl/EGL, host UDP server), not a direct-PAL test |
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
