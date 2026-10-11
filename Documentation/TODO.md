# TODO — aarch64 VM PAL (`gramine-vm`)

Open items as of 2026-10-10 (HEAD `fc009987`). Context: `Documentation/devel/aarch64/vm-pal-plan.md` (history),
`x86-parity-vm.md` (results), `Documentation/devel/vm.md` (how to run). Test ids refer to
`libos/test/regression/test_libos.py`; the skip/xfail entries live in
`libos/test/regression/vm_skip.toml`.

## 1. Implement `_PalThreadResume()` (shared VM PAL, also affects x86)

**Problem.** `pal/src/host/vm/pal_threading.c: _PalThreadResume()` is a stub returning
`-PAL_ERROR_NOTIMPLEMENTED`. LibOS uses it to interrupt a thread that is blocked in a syscall when an
asynchronous signal arrives (`libos/src/bookkeep/libos_signal.c:733`, `libos/src/sys/libos_exit.c:214`,
`libos/src/sys/libos_sigaction.c:472`). Without it the blocked thread is never woken.

**Symptoms.** Three tests hang until the harness timeout (`[VM:no-thread-resume]` skips):
`test_060_sigaltstack` (`alarm` + `pause()`), `test_150_itimer`, `test_404_sigterm_multithread`
(the last also needs a host `kill` into the guest, which is a separate limitation). Repro:
`GRAMINE_VM=1 gramine-vm sigaltstack` (after `gramine-test build sigaltstack`) prints
"OK on sigaltstack in main thread before alarm" and then hangs in `pause()`.

**Sketch.** Mark the target thread "interrupted" and wake it out of `sched_thread_wait()` /
`PalEventWait` so its blocking PAL call returns `-PAL_ERROR_INTERRUPTED`; if the target is running on
another CPU, kick it (SGI) so it re-checks at its next syscall boundary. Check how
`pal_common_events.c` / `pal_common_object.c` block (`g_streams_waiting_events_futex`) and what
interruptibility they need. aarch64 note: LibOS runs at EL1 under this PAL, and signals are only
delivered at syscall return (`vm_signal_return_trampoline`), so a thread spinning in EL0 will not see a
signal until it next enters the kernel — decide whether that is acceptable.

**Done when.** The three tests pass without their skip entries, and `x86` behaviour is unchanged or
improved (the stub is shared code).

## 2. Finer timer granularity

**Problem.** Timed waits (`usleep`, futex/poll/event timeouts) are serviced only from CPU0's tick
(`notify_about_timeouts_uninterruptable()` in `kernel_time.c`, called from the tick handler in
`arch/aarch64/kernel_interrupts.c`), and the tick is a fixed 100 ms (the x86 LAPIC constant).
Every timed wake-up therefore costs up to 100 ms.

**Symptoms.** `test_001_rwlock` runs ~0.5 s per iteration (22 s for 10 iterations, 57 s for 100) and
exceeds any timeout (`[VM:slow-timer]` skip). Repro: `GRAMINE_VM=1 gramine-vm rwlock 1 10 2 1 100`.
Also makes every `sleep`-heavy example slow.

**Options.** (a) Program the timer for the earliest pending timeout (one-shot) instead of / in
addition to the periodic tick: `register_timeout()` knows the deadline; `lapic_timer_rearm()` is the
re-arm point. (b) Simply shorten the tick (10 ms) — easy, costs interrupts and the EL0 preemption
quantum shrinks with it, so check `hello`/`spinlock` stability afterwards. Timeouts are handled on CPU0
only; decide whether that stays.

**Done when.** `rwlock` (`5 100 10 3 100`) finishes well inside the 40 s test timeout and the
`slow-timer` entry can be removed; `hello` (SMP=2/4, 150 runs) and `spinlock` still pass.

## 3. The xfails (`[VM:investigate]`)

Cause not yet understood; each is a plausible real bug. (`test_021_getdents_large_dir`, a 3000-entry
virtiofs directory read, is also listed: it timed out in one run and passed in another.)

- **`test_023_readdir`** — `readdir failed: Bad file descriptor`,
  `file __testfile__ was not found: Bad file descriptor`, exit 1. Fails 0/20 standalone but passed inside one full-suite run (state/ordering
  dependent); building with `-Dgraphics=disabled` does not fix it. Looks like `getdents`/`open` on a
  virtiofs directory returning `EBADF`. Start in `pal/src/host/vm-common/pal_common_files.c` and the
  virtio-fs directory path. Repro: `gramine-test build readdir; GRAMINE_VM=1 gramine-vm readdir`.
- **`test_030_fdleak`** — `error at open(fname, O_RDONLY) (line 31): Permission denied`, exit 1
  (also a spurious QEMU `virtio_bus_start_ioeventfd: failed` line in the run — check it is unrelated).
  Possibly the test's temp-file handling on the shared filesystem (mode bits / uid under virtiofsd
  `--sandbox none`).
- **`test_092_sighandler_sigpipe`** — the test expects a nonzero exit (SIGPIPE terminates the process)
  but the guest exits 0. Check whether the write to a closed pipe reaches `EPIPE`/SIGPIPE at all in the
  single-process PAL, and how the VM PAL reports a signal-killed process (`libos_clean_and_exit` ->
  `_PalProcessExit` code vs the launcher's exit-code recovery, which uses the console line
  `[ VM exited with code N ]`).

When one is fixed, delete its `vm_skip.toml` entry (an xfail that starts passing shows up as XPASS).

## Smaller known gaps (not scheduled)

- **Latent bug masked by the `TRACE` breadcrumb.** The C-level probes are gone; what remains is the
  `TRACE` asm macro (`kernel_context.S`), the `dbg_irq_trace` per-CPU field and its offset. Removing the
  `TRACE 1` at the `el1_irq_entry` entry (asm only, or also the field) makes `sched_set_get_affinity`
  panic deterministically (LibOS `Reference count dropped below 0` at `libos_dcache.c:132`, opening
  `/sys/devices/system/cpu/online`) and 5 suite tests fail (OpenMP simple_for_loop, cpuaffinity x2,
  flock_lock, procstat). Not layout/timing (5 `nop`s fail), not `x29`/`x30` alone, not the stored values
  (zero stores fail). Find the real cause (gdb on the gdbstub around `el1_irq_entry`), then drop `TRACE`.
  The stash `probes3` (`git stash list`) holds older probe work.
- **AF_INET sockets trip an assert** in `kernel_virtio_vsock.c:709` (`idx < g_vsock->conns_size`)
  instead of failing with `EAFNOSUPPORT`; the 8 `[VM:no-tcpip]` tests hit it. A clean error would be
  better than a kernel assert.
- **`dev:` device streams, `/sys`, `loader.log_file`, `/dev/attestation`** are not provided by the VM PAL
  (`no-dev-streams` 4 tests, `no-sysfs`, `no-log-file`, `no-attestation`). Decide which are worth
  implementing.
- **`large_mmap`** needs a larger page-table pool / block (2 MB) mappings instead of 4 KB pages
  (`PT_POOL_TABLES` is 2048 tables ≈ 4 GB) and a guest with > 6 GB RAM.
- **Python CI example** needs a PIE `python3` (Ubuntu's is a non-PIE `ET_EXEC` at 0x400000); the
  example runner currently skips it.
- **Launcher environment**: `gramine-vm` is a bash script, so it cannot forward the caller's environment
  faithfully (`PWD`, `_`, `SHLVL` are rewritten); `test_103_env_from_host` is skipped for that reason.
  The guest hostname is always `localhost`.
- **Minimal install without the bundled glibc (`-Dlibc=none`).** The default `-Dlibc=glibc` builds glibc 2.39
  (the slowest part of a fresh build) only to provide the guest's `/lib` runtime directory and loader
  (`gramine.runtimedir()`); on aarch64 it carries no fast path (the patched syscall path is x86-only, and vm/tdx
  builds use `compile-no-patches.sh`). Try `-Dlibc=none` and see what survives: manifests that mount
  `gramine.runtimedir()` (most tests and examples) would need to mount the host's `/lib/aarch64-linux-gnu`
  instead, and results then depend on the host glibc. Decide whether a "minimal" install mode (faster build,
  host libc) is worth offering. Untested.
- **x86_64 VM PAL does not forward CPU exceptions.** A divide error (`#DE`) panics the guest
  (`unhandled exception (vector nr. 0)`) instead of reaching LibOS as `SIGFPE`; `test_093_sighandler_divbyzero`
  is xfailed on x86_64. The aarch64 PAL delivers its faults, so compare `pal_common_exception.c` /
  `kernel_interrupts.c` for the vectors the x86 kernel forwards (page faults only today).
- **x86_64 `test_010_gramine_run_test`** hits `Internal memory fault with VMA at 0x17ce00` in LibOS under the VM PAL
  (xfailed on x86_64, passes on aarch64); not investigated.
- **x86_64 guest boot is ~12 s per run** on a hosted runner (8 GB guest), which makes the LibOS suite ~18 minutes.
  Check where the time goes (QEMU memory backends, BIOS, page-table setup) before shrinking `GRAMINE_RAM_SIZE`.

## Cross-checks waiting on x86 / other QEMU

- Fill the `TBD (x86)` cells in `x86-parity-vm.md`; confirm on x86 that (a) non-PIE binaries
  work (no `tests_pie` needed) and (b) `gramine-vm` propagates the application's exit status (the
  aarch64 launcher recovers it from the console; the x86 launcher `exec`s QEMU — unverified).
- QEMU: the aarch64 numbers use stock Ubuntu QEMU 8.2.2; the repo's fork (10.1.50) does not build for
  aarch64 (`egl-redirect/xcb.c` x86 asm, `util/sg-listener.c` `KVM_VCPU_TSC_CTRL`). To rule out a QEMU
  version effect, build a newer stock QEMU (10.x) in the container and run the suite with
  `QEMU_PATH=...`; porting the fork's two x86-only files is the alternative.

## Notes for this tree (gramine-multiarch)

- The aarch64 `vm` PAL must be built with `-Dgraphics=disabled` (`scripts/install.sh --vm` does it): the
  graphics redirection assumes the QEMU fork's host agent, which does not exist for aarch64.
- The aarch64 VM-only guard macro is `PAL_HOST_VM` (not `HOST_TYPE`, which is defined for every PAL flavor).
- The x86_64 builds were not run from this branch (no x86 host); the shared sources were type-checked
  against the x86_64 headers on aarch64 and the aarch64 direct PAL suites were re-run (145/36, 28/8).
  A real x86_64 `vm`/`tdx` build and test run is still to be done.
