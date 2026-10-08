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

Totals match (PAL 36, LibOS 176), so no test is unaccounted for. Both architectures have zero failures.

## Fixes found while running on x86_64
- `pal/regression/send_handle.c`: `memcmp` over-read past the `MSG` literal (failed on x86_64 depending on `.rodata` layout). It now compares against a zero-padded copy.
- `pal/regression/memory.c`: the arch-neutral helpers moved out of the arch `#if` block so the file builds on x86_64.
- `pal/include/arch/x86_64/pal_arch.h`: added a no-op `pal_tcb_singleton_snapshot_before_user_entry()` to match the aarch64 hook.

## TODO
- aarch64 skip lists (run the commands in the next section on aarch64 and fill the aarch64 column).

## Skip lists

Collected with `python3 -m pytest -q -rs` in `pal/regression` and `libos/test/regression`
(needs `PYTHONPATH` and `PATH` pointing at the Gramine install). Line numbers refer to
`test_pal.py` / `test_libos.py`.

### PAL regression

| Reason | x86_64 | aarch64 |
|---|---|---|
| Needs SGX (`test_pal.py` lines 177, 466, 480, 550) | 4 | TODO |
| Other | 0 | TODO (expected 4 more than x86_64) |
| **Total skipped** | **4** | 8 (from Milestone A) |

### LibOS regression

| Reason | x86_64 | aarch64 |
|---|---|---|
| SGX only (file-check-policy, attestation, trusted/sealed files, CPUID/RDTSC emulation, MRENCLAVE, SIGSTRUCT) | 23 | TODO |
| Needs `UBSAN=1` / `ASAN=1` (lines 49, 53, 56, 59) | 4 | TODO |
| Needs debug build (lines 1362, 1400) | 2 | TODO |
| Needs `/dev/gramine_test_dev` (lines 1146, 1151, 1161) | 3 | TODO |
| Other (arch-specific) | 0 | TODO (expected 4 more than x86_64) |
| **Total skipped** | **32** | 36 (from Milestone A) |

Every x86_64 skip is an environment or SGX skip, so none is an x86_64 gap. The remaining
aarch64 skips should be the x86-only tests, which would explain the gap of 4 in each suite.
Confirm with the aarch64 run.

### Commands for the aarch64 run
```sh
export PYTHONPATH=<prefix>/lib/python3.X/site-packages PATH=<prefix>/bin:$PATH
cd gramine-tdx/pal/regression && python3 -m pytest -q -rs 2>&1 | grep -a '^SKIPPED\|passed'
cd ../../../libos/test/regression && python3 -m pytest -q -rs 2>&1 | grep -a '^SKIPPED\|passed'
```
