# Performance benchmarks (direct PAL, native vs gramine-direct)

```
./benchmarks/run_benchmarks.sh            # syscall microbenchmark + redis-benchmark
./benchmarks/run_benchmarks.sh syscall    # CI-Examples/syscall-bench only
./benchmarks/run_benchmarks.sh redis      # redis-server native vs gramine-direct
```

Results are appended to `benchmarks/results/<date>_<host>_<arch>.txt` with a machine/git header.
Run the same settings before and after a change and compare. Server and clients are pinned with
`taskset`. Redis must be built first (`./run_all_tests.sh redis`). Env knobs are listed in the
script header.

## Baseline: seccomp-trap syscall path (no glibc patch, no call-site rewriting)

Box: Qualcomm aarch64, 8 cores, 7.6 GiB, Ubuntu 24.04 under WSL2 (6.18.40.1-microsoft-standard-WSL2),
commit ee845f5f, 2026-10-08. WSL2 is virtualized, so signal delivery may cost more than on bare
metal; re-run on the GX10 for a bare-metal baseline.

**Syscall cost (ns/call, min of 7)**: native vs gramine-direct

| syscall | native | gramine-direct |
|---|---|---|
| getpid | 126 | ~1230-1280 |
| getuid | 122 | ~1280-1310 |
| read(/dev/zero,1) | 161 | ~1340-1350 |
| write(/dev/null,1) | 150 | ~1310-1360 |
| clock_gettime (raw) | 192 | ~1535-1550 |

The extra cost is a near-constant ~1.1-1.4 us per syscall (seccomp trap + SIGSYS frame +
`rt_sigreturn`), independent of what the call does.

**redis-benchmark** (requests/s, 200k requests, 50 clients, 3 runs; Redis 6.0.5)

| test | native | gramine-direct | gramine/native |
|---|---|---|---|
| SET/GET/INCR/LPUSH/LPOP/HSET, `-P 1` | ~104k-118k | ~74k-80k | ~0.68 |
| SET, `-P 16` | ~1.06M | ~0.23M-0.24M | ~0.22 |
| GET, `-P 16` | ~0.99M-1.16M | ~0.23M-0.25M | ~0.21 |

With pipelining the server is syscall-bound, so the trap cost shows fully (~4.5x slower).

**Known noise:** in every gramine-direct redis run exactly one of the six `-P 1` tests drops to
~36k req/s (about half; a different test each time: LPOP, INCR, GET). Native never does. Likely a
periodic stall; not yet investigated. Compare medians or exclude that outlier.

## With `sys.rewrite_syscalls` (lazy `svc` site rewriting, default on)

Same box/settings; results file `results/2026-10-08_testhost_aarch64_rewrite.txt`. First trap at
each `svc #0` site rewrites it to a `b` into a per-site stub (`libos/src/arch/aarch64/
libos_syscall_patch.c`), so later executions skip the seccomp trap. No libc changes needed.

| syscall (ns/call, min of 7) | native | trap path (baseline) | rewritten |
|---|---|---|---|
| getpid | 126 | ~1230-1280 | 42 |
| getuid | 122 | ~1280-1310 | 67 |
| read(/dev/zero,1) | 161 | ~1340-1350 | 129 |
| write(/dev/null,1) | 150 | ~1310-1360 | 133 |
| clock_gettime (raw) | 192 | ~1535-1550 | 319 |

(getpid/getuid beat native because the LibOS answers them without entering the host kernel.)

| redis (req/s) | native | trap path | rewritten |
|---|---|---|---|
| `-P 1` GET/INCR/LPUSH/LPOP/HSET | ~101k-115k | ~74k-80k | ~144k-146k |
| `-P 16` SET | ~0.84M-1.06M | ~0.23M | ~0.79M |
| `-P 16` GET | ~1.1M | ~0.23M-0.25M | ~0.84M |

Treat the `-P 1` result with caution: gramine-direct exceeding native is surprising and is not
explained yet (likely client/loopback/scheduling effects on WSL2); the intermittent single-test
dip described above is still present. Compare against the same-run native numbers, and rerun on
bare metal (GX10) before drawing conclusions.

A/B switch: `sys.rewrite_syscalls = false` in a manifest restores the trap path
(`make REWRITE=0` in `CI-Examples/syscall-bench`).
