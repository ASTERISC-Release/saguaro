# Benchmarks

Performance measurements for the direct PAL: native execution versus `saguaro-direct`.

```
./test/benchmarks/run_benchmarks.sh            # syscall microbenchmark + redis-benchmark
./test/benchmarks/run_benchmarks.sh syscall    # per-syscall cost only
./test/benchmarks/run_benchmarks.sh redis      # redis-server native vs saguaro-direct
```

Outputs are appended to `test/benchmarks/results/<date>_<arch>.txt` (git-ignored). Each file starts with a
header recording the kernel, CPU, memory, OS and git revision of the machine it ran on, so runs can
be compared before and after a change. Compare the min/median of repeated runs, not single runs;
server and client are pinned with `taskset` (see the script header for the environment knobs).
The Redis mode needs Redis built first (`test/direct/test-direct.sh redis`); no Redis results are published
here.

## Layout

- `run_benchmarks.sh` — the driver: builds and runs the workloads natively and under
  `saguaro-direct` and writes the results file.
- `syscall-bench/` — the per-syscall microbenchmark (`syscall_bench.c`, manifest template,
  Makefile; `make REWRITE=1|0` forces `sys.rewrite_syscalls` on/off in its manifest).

## Syscall cost

Measured on AArch64 under WSL2. WSL2 is virtualized, so signal delivery (and therefore the seccomp-trap
path) may cost more than on bare metal: read the absolute numbers as indicative and the ratios as
the result. No bare-metal numbers are published yet.

Nanoseconds per call, minimum over 7 repetitions of 300,000 calls:

| syscall | native | seccomp-trap path | with syscall-site rewriting |
|---|---|---|---|
| `getpid` | 126 | ~1230-1280 | 42 |
| `getuid` | 122 | ~1280-1310 | 67 |
| `read(/dev/zero, 1)` | 161 | ~1340-1350 | 129 |
| `write(/dev/null, 1)` | 150 | ~1310-1360 | 133 |
| `clock_gettime` (raw syscall) | 192 | ~1535-1550 | 319 |

Without a patched libc every guest `svc #0` is caught by a seccomp trap, which costs a near-constant
~1.1-1.4 us per call regardless of what the call does. With `sys.rewrite_syscalls` (the default), the
first trap at each `svc` site rewrites it into a branch to a per-site stub that enters the LibOS
directly (`libos/src/arch/aarch64/libos_syscall_patch.c`), so later executions skip the trap.
`getpid` and `getuid` come out faster than native because the LibOS answers them without entering
the host kernel. To measure the trap path, set `sys.rewrite_syscalls = false` in the manifest
(`make REWRITE=0` in `test/benchmarks/syscall-bench`).
