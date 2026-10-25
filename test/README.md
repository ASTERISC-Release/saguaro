# test/: the regression tiers and the test tree

One command runs the regression set of a variant:

    test/quick.sh <direct|vm|sev|sev-snp|sgx|vbs|tdx> [--tier smoke|regression|extended]
                  [--out DIR] [--vcpus N] [--list] [--dry-run] [--only STEP,...]

The same steps have the same names on every variant, the output has one format, and the exit status is 0
only when nothing failed unexpectedly. `--list` prints the steps of a variant and tier, `--dry-run` prints
their commands, `--help` the options. Run it from the repository root with the variant installed
(`scripts/install.sh` for `direct`, `scripts/install.sh --vm` for `vm`, `--sev` for `sev` and `sev-snp`, `--sgx`
for `sgx`; the prefix is `SAGUARO_PREFIX`, or `SAGUARO_DIRECT_PREFIX`, `SAGUARO_VM_PREFIX`, `SAGUARO_SGX_PREFIX`).
The VM variants need a user that may run `saguaro-vm` (root in the container of `container/run.sh`).

## The three tiers

| Tier | Budget (the test run only: the install, the build and the RISC-V guest boot are not counted) | What runs |
|---|---|---|
| `smoke` | about 3 minutes | install check, `check-core-list`, `helloworld`, the `unit_tests` examples (mremap, thread-tls, thread-rwlock, thread-stress, networks), two LibOS tests (`202_fork_and_exec`, `600_multi_pthread`). vbs: no LibOS tests, and every `unit_tests` program is skipped (no VBS build). tdx: the compile job. |
| `regression` (the default) | 20 minutes per variant | `smoke`, then `pal` (direct and sgx: the PAL regression), `libos-core` (the 127 ids of `test/quick/core-tests.txt`), the examples `bash sqlite python busybox memcached redis lighttpd rust nginx` through their `exec.sh`, and the steps of the variant (below). |
| `extended` | hours; nightly or by hand | the full LibOS suite, the vCPU sweep, the loops, the stress tests, `libos/test/fs`, `libos/test/abi`, `test/unit`, every example that has an `exec.sh`, LTP. |

Measured on a development PC (x86_64, 24 threads, KVM, examples already built): `direct` smoke 18 s,
`direct` regression 38 s (the PAL suite 9 s, `libos-core` 8 s, `unit_tests` 16 s; the nine examples about 1 s
each except `python` 3 s); `vm` smoke 26 s, `vm` regression 267 to 296 s (`libos-core` 115 s, `vm-net` 82 to
115 s, `vm-multiproc` 26 s; the nine examples 1 s each except `sqlite` 3 s and `python` 7 s). The first run also
builds the examples (busybox, memcached, nginx and lighttpd download and compile their sources: 62 s for the
first `direct` regression run, of which `lighttpd` 23 s). `vm` extended steps: the full LibOS suite 167 s at 2
vCPUs (244 s at 1, 167 s at 4), `vm-stress` 163 s, `vm-sig-loop` 157 s, `vm-boot-loop` 169 s, `libos-fs` 61 s.
On an AMD SEV host (8G guests, examples already built unless stated): `sev-snp` smoke 46 s, `sev-snp` regression
907 s including the first build of the examples (about 200 s; `libos-core` 431 s, `sev-suite` 103 s,
`vm-multiproc` 61 s); plain `sev` smoke 78 s, `sev` regression 1362 s (`libos-core` 961 s, `sev-suite` 113 s,
`vm-multiproc` 103 s), over the 20-minute budget: a plain-SEV guest takes about 7.5 s to start at 8G (3.3 s under
SNP), and `libos-core` is where it adds up. Extended `sev-snp`: `libos-full-2vcpu` 617 s, `vm-stress` 181 s.

`sev` and `sev-snp` under `SAGUARO_ACCEL=tcg` (the emulated SEV of QEMU TCG, below; the results say `sev-emu`): `smoke` adds the SEV
smoke suite of the mode (`sev-smoke`: hello at 1, 2 and 4 vCPUs and the vsock
loopback), `regression` adds only `libos-core`; the examples and `vm-multiproc` are in its `extended` tier.
Measured (QEMU TCG, 2 vCPUs, examples built): `sev-emu` smoke 42 s (`sev-smoke` 9 s, `unit_tests` 16 s), `sev-emu` regression 338 s (`libos-core` 294 s: 120 pass, 4 skip,
2 xfail, 1 known). A hosted 4-vCPU runner is about twice as slow per boot, so the regression tier is expected
to take 10 to 12 minutes there; `QUICK_PYTEST_K` can leave out the slowest tests (`test_401_exit_group` 12 s,
`test_220_send_handle`, `test_076_timerfd`, `test_110_fcntl_lock`, `test_041_futex_timeout`) if a shorter
run is wanted.

The VM-based variants (`vm`, `sev`, `sev-snp`) run `smoke` and `regression` with exactly 2 vCPUs
(`SAGUARO_CPU_NUM=2`; `--vcpus` changes it) and 8G of RAM (`SAGUARO_RAM_SIZE`): there is no sweep in these
tiers. The `extended` tier keeps it, and the step names say the count: `libos-full-2vcpu`,
`libos-sweep-1vcpu`, `libos-sweep-4vcpu`.

Steps of the variants in `regression`, besides the common ones:

| Variant | Extra steps |
|---|---|
| `direct` | `unit` (`test/unit`) |
| `vm` | `vm-net` (`test/vm/test-vm.sh loopback outbound`), `vm-multiproc` (`test/vm/multiproc/check-probes.sh spawn signals forks misc sysv`; x86_64 only) |
| `sev`, `sev-snp` | `sev-suite` (`test/sev/test-sev.sh --vcpus 2`), `vm-multiproc`; `sev-snp` also `libos-snp-attest` |
| `sev`, `sev-snp` under `SAGUARO_ACCEL=tcg` | none in `regression` (the hosted-runner budget); `smoke` has `sev-smoke`, `extended` the nine examples, `vm-multiproc` and `sev-smoke-1vcpu` |
| `sgx` | `libos-sgx-only` (file check policy, CPUID and RDTSC under SGX, sealed and MRENCLAVE tests), `unit` (with `test_sgx_sign.py`) |
| `vbs` | `vbs-regress` (the core programs that have a VBS case, `test/quick/vbs-core.txt`, one result row per case; the failures that need `fork` or host devices are known failures), the examples `busybox sqlite python redis lighttpd rust` and `CI-Examples/vbs`; no LibOS pytest |
| `tdx` | `tdx-build` only: the TDX PAL cannot run without TDX hardware |

`extended` adds `libos-fs` (`libos/test/fs`), `libos-abi` (`libos/test/abi`, x86_64 direct), `ex-<name>` for
every other example, `stress-rewrite` (aarch64 and riscv64 only: x86_64 does not rewrite system calls, so its `blob` scenario fails there by design) and `prefix-sweep` (direct), and on the VM variants `vm-net-all`,
`vm-multiproc-all` (the probes `libos examples redis` as well), `vm-sig-loop`, `vm-boot-loop`, `vm-stress`
(below) and `ltp`. An example that does not offer the variant is reported as `skip`.

Two steps are left out of the tiers on purpose. `guest-loopback` (in `vm-net-all`) hung for its 120 s limit on a hosted
x86_64 runner, where `loopback` and `outbound` passed; the cause is not known, and it passes on a development PC. Under
QEMU software emulation (TCG, the aarch64 `vm` on a host without KVM) the `thread-stress` program of `unit_tests` needs
about 15 minutes, so `smoke` and `regression` skip it there (`UNIT_TESTS_SKIP`, `CI-Examples/unit_tests/exec.sh`) and
`extended` runs it as `ex-thread-stress`.

`test/quick.sh direct --target riscv64` runs the direct variant in the RISC-V guest of
`scripts/riscv64/guest.sh` (inside the cross container; `Documentation/quickstart/riscv.md`).

## The emulated SEV (`SAGUARO_ACCEL=tcg`)

`SAGUARO_ACCEL=tcg QEMU_PATH=<qemu> test/quick.sh sev|sev-snp` runs the SEV PAL (`saguaro-sev`, the same install as `sev`) on a host without SEV
hardware and without KVM: QEMU's software emulation (TCG) with the `sev-emu` and `sev-snp-emu` objects of the
`dev-sev` branch of the lab's QEMU fork, a *functional* emulation of what an SEV, SEV-ES or SEV-SNP guest can
observe: `SEV_STATUS` and CPUID 0x8000001F, the C-bit in page tables (bookkeeping only), `#VC` for the
intercepted instructions, the GHCB MSR protocol and the VMGEXIT events, the SNP CPUID and secrets pages,
`PVALIDATE`, `RMPADJUST` and page-state changes with the "page not validated" `#VC`, AP creation from a
VMSA. It has no firmware and no RMP: there is **no attestation** (a guest request fails as a firmware error
does, `/dev/attestation/quote` cannot be read), **no memory isolation or encryption** (the host reads guest
memory), no launch measurement that a report could be checked against, and no VMPL model. It validates the
PAL's use of the protocols and finds regressions in that code; it does not replace a run on an SEV host,
which stays the qualification of a release (`test/quick.sh sev` and `sev-snp` there). `QEMU_PATH`
names the emulating QEMU (`qemu-system-x86_64` of that branch, built with `--enable-igvm`); it is checked
first by `CI-Examples/common_tools/sev-emu-check.sh` (set, a QEMU, TCG, the `sev-emu` or `sev-snp-emu` object), and
`exec.sh --sev|--sev-snp` and `test/sev/test-sev.sh` run the same check. The mode follows the variant. The known differences are in `expected-failures.toml` (`test_020_cpuinfo`: the guest sees
QEMU's `max` CPU model) and `test/sev/test-sev.sh` skips its `memory`, `measure` and `attest` suites under
`SAGUARO_ACCEL=tcg`.

## Results

`--out DIR` (default `/tmp/saguaro-quick/<variant>-<arch>-<date>`) receives:

- `results.csv`, one row per test (a pytest step, from its JUnit file), per example (the `[PASS]` lines of
  `exec.sh`) or per step: `variant,arch,commit,tier,step,test,result,seconds,vcpus`. `result` is `pass`,
  `fail`, `skip`, `xfail`, `xpass` (a test that `vm_skip.toml` expects to fail passed; it counts as a pass in
  `summary.txt`, which also gives `xpass=N`, and as an xfail in the comparison with `expected.csv`) or `known-fail`; `vcpus` is `-` for a variant without a vCPU count.
- `summary.txt`, one line: `QUICK variant=vm arch=x86_64 tier=regression vcpus=2 commit=... pass=N fail=N skip=N xfail=N known=N secs=N status=PASS`.
  Keep it for a release or a pull request, in particular for the variants without CI (sev, sev-snp, sgx, vbs).
- `<step>.log` and `<step>.xml`: the raw output and the JUnit file of every step.

The exit status is 1 if a test or a step failed unexpectedly. A failure listed in
`test/quick/expected-failures.toml` (matched exactly on variant, architecture, vCPU count, step and test id) is
`known-fail` and does not fail the run; every entry has a reason. `test/quick/expected.csv` has the counts the
`libos-core` step gives per variant; for a measured row the run prints a `NOTE` when the counts differ.
Skips and xfails under the VM PAL come from `libos/test/regression/vm_skip.toml`, as before.
The seven SGX entries (`*_chroot` tests that need a file symlink or `RENAME_EXCHANGE`, and the two UNIX socket tests) are
gaps of the SGX PAL by design of the enclave threat model (it refuses host-visible nodes and does not pass file
descriptors through the host), not bugs. The helloworld example of sgx and vbs fails about 1 run in 8 with `mmap
DATA_REGION: Cannot allocate memory` (a 512 MiB mapping in a 1G enclave); `quick.sh` runs that one step a second time
in that case only, prints `NOTE retried` and records `retried=1` in `results.csv` and `summary.txt`.

## Files

| File | Role |
|---|---|
| `test/quick.sh` | the entry point |
| `test/quick/core-tests.txt` | the LibOS tests of the `regression` tier, one pytest id per line, the same on every variant |
| `test/quick/check-core-list.py` | checks the list against `test_libos.py` without pytest (no SGX, ASAN, AVX, device or architecture-only test, no unknown id); a `smoke` step |
| `test/quick/expected-failures.toml`, `expected.csv` | known failures and expected counts (see above) |
| `test/quick/report.py` | writes `results.csv` and `summary.txt` for `quick.sh` |
| `test/quick/vbs-core.txt` | the core tests that have a VBS case |
| `libos/test/regression/conftest.py` | `SAGUARO_TESTSET=core` keeps only the ids of the core list (no pytest markers; the test functions are unchanged) |

A test joins the core list by its id in `test/quick/core-tests.txt` (then run
`python3 test/quick/check-core-list.py`). A test that fails on a variant for a known reason goes to
`expected-failures.toml`; a flaky test leaves the core list. A test that is too slow on one host is left out of
a run with the pytest expression in `QUICK_PYTEST_K`, which the workflow of that host sets
(`.github/workflows/`).

## The runners the tiers call

| Command | Role |
|---|---|
| `CI-Examples/<name>/exec.sh --<variant>` | the single runner of the examples (`--direct --vm --sev --sev-snp --sgx --vbs`) |
| `test/direct/test-direct.sh [pal] [libos]` | the PAL suite and the full LibOS suite of the direct PAL. Its example suites (`helloworld multithread busybox bash redis memcached sqlite python lighttpd nginx rust networks`) are deprecated and stay only because `scripts/riscv64/run-examples.sh` runs them in the RISC-V guest |
| `test/vm/test-vm.sh [libos] [loopback] [epoll] [outbound] [udp] [guest-loopback]` | the LibOS suite under `saguaro-vm` (2 vCPUs unless `SAGUARO_CPU_NUM` says otherwise) and the VM network suites |
| `test/vm/multiproc/check-probes.sh [check...]` | the multi-process checks of the VM PAL: `spawn signals forks misc sysv libos examples redis` (default: the first five) |
| `test/vm/multiproc/sig-loop.sh`, `boot-loop.sh` | the signal loop and the boot loop (extended) |
| `test/vm/stress/run-stress.sh` | the concurrency stress tests (extended, below) |
| `test/sev/test-sev.sh [--vcpus N] [suite...]` | the SEV suites hello, memory, vsock, measure, attest (without `--vcpus`: hello at 1 and 4 vCPUs, the others at 1) |
| `test/sev/test-sev-all.sh` | the SEV qualification run by hand: build, `test-sev.sh` in both modes, the full LibOS suite at one vCPU count, examples, probes |
| `test/unit/` | pytest unit tests of `saguarolibos` |
| `libos/test/fs`, `libos/test/abi/x86_64` | the file system and process-entry (ABI) suites, run with `saguaro-test pytest` in their folder (extended; the ABI suite exists for x86_64 only) |

## Stress tests (`test/vm/stress/`)

Three plain pthread and futex programs and a runner. `hangrepro` creates and joins threads, runs a futex ring,
timed waits, and concurrent thread and page-table creation. `idlebench` runs a futex ring with random sleeps,
mmap/munmap churn, short-lived threads and a pinned waiter, with a stall detector. `madv_stress` discards
thread stacks and a large region with `MADV_DONTNEED` while other threads map and unmap. `run-stress.sh` builds
them, renders `manifest.template` once per program and runs eight cases under `saguaro-vm` (`--list`, `-n
ROUNDS`, `STRESS_SCALE`). A case passes with exit status 0 and its `... OK` line; a stall or a hang fails it.
The cases check the concurrency fixes of the VM PAL: lost wake-ups and timeouts, page tables created
concurrently, an exiting thread's stack, and the discard of pages on `MADV_DONTNEED`.

## Other tests in this folder

| Path | Role |
|---|---|
| `test/direct/direct-prefix-sweep.sh` | `fork_and_exec` from install copies at many path lengths (extended, direct) |
| `test/vm/network/` | the VM network tests (`test-vm-loopback.sh`, `-epoll`, `-outbound`, `-udp`, `-guest-loopback`, `-redis`, `test-vm-relay.py`) |
| `test/vm/vsock-loopback/` | programs and servers of the VM network tests |
| `test/vm/multiproc-aarch64/` | the AArch64 multi-process scripts (`hello.sh`, `probes.sh`, `load.sh`, LTP scripts, hang tools), run by hand |
| `test/vm/timer/` | timer measurements (`measure.sh`, `measure-sev.sh`) |
| `test/benchmarks/` | native against `saguaro-direct` |
| `test/boot-probe.sh` | boot time and memory commit of a graphics VM |
| `test/*-syscall-profile.py` | checks and calibration of `scripts/syscall-profile.py` |
| `test/vulkan/`, `test/proton-vm/` | graphics probes and Proton/Wine scripts (need a display) |
| `test/vbs/` | VBS enclave spikes (Windows) |
| `test/sev/` | `sev_probe`, `sev_attest`, `qmp.py`, `sev_memcheck.py`, used by `test-sev.sh` |

## CI

All workflows are manual (`workflow_dispatch`) and call `test/quick.sh` after their build steps:
`direct-x86_64.yml` and `direct-aarch64.yml` (`direct`), `direct-riscv64-emulated.yml` (`direct --target
riscv64`), `vm-x86_64.yml` (`vm`; it also runs `test/vm/network/test-vm-redis.sh` for the inbound relay) and
`vm-aarch64-tcg.yml` (`vm` under QEMU TCG). Each has a `tier` input (default `regression`), writes `summary.txt` to
the job summary and uploads the output directory. Deselects for a slow host are `QUICK_PYTEST_K` in the
workflow (aarch64 TCG: `test_604_sigreturn_regs`; riscv64: `test_103_gettimeofday`, `test_603_rewrite_stress`)
and entries of `expected-failures.toml`. The TDX PAL is compiled by the `tdx-build` job of `direct-x86_64.yml`.
SEV, SGX and VBS have no CI (the repository is public): run `test/quick.sh sev`, `sev-snp`, `sgx` or `vbs` by
hand per release and keep the summary line. The emulated SEV can run on a hosted x86_64 runner once
the emulating QEMU is available to the workflow (the `dev-sev` fork is private: publish it or build the binary
elsewhere and fetch it as an artifact); its results say `sev-emu` and are not a substitute for the SEV host.
