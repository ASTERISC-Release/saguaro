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
| redis | not run | Pass | Not part of the x86_64 parity run |
| helloworld / multi-thread | not run | Pass | Not part of the x86_64 parity run |

Totals match (PAL 36, LibOS 176), so no test is unaccounted for. Both architectures have zero failures.

## Fixes found while running on x86_64
- `pal/regression/send_handle.c`: `memcmp` over-read past the `MSG` literal (failed on x86_64 depending on `.rodata` layout). It now compares against a zero-padded copy.
- `pal/regression/memory.c`: the arch-neutral helpers moved out of the arch `#if` block so the file builds on x86_64.
- `pal/include/arch/x86_64/pal_arch.h`: added a no-op `pal_tcb_singleton_snapshot_before_user_entry()` to match the aarch64 hook.

## TODO
- Per-test skip lists (rerun with `pytest -rs`) to show which tests skip on each side.
- Run redis, helloworld and multi-thread on x86_64.
