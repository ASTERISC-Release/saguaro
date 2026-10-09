# aarch64 port of Gramine (gramine-tdx) — Milestone A: the `linux`/`direct` PAL

**Status: COMPLETE.** Full libos regression suite green on aarch64 (**140 passed / 0
failed / 36 skipped**), PAL regression green (**28 passed / 8 skipped**), plus real-world
example applications (redis, memcached, busybox, multi-thread) validated over real TCP.
The follow-on plan — porting the `vm`-PAL unikernel, the actual end goal — lives in
`ARM64_MILESTONE_B.md`.

## Validated platforms

Milestone A (direct PAL, `-Ddirect=enabled -Dvm=disabled -Dsgx=disabled -Dtdx=disabled
-Dlibc=glibc`, native build) has been built and run through `run_all_tests.sh` on:

| | Original dev box: ASUS Ascent GX10 (NVIDIA GB10) | Second box (2026-10-08) |
|---|---|---|
| CPU | NVIDIA GB10, 20 Arm cores (10x Cortex-X925 + 10x Cortex-A725, per vendor spec) | Qualcomm aarch64, 8 cores (ASIMD/AES/PMULL/SHA/CRC32/atomics; no SVE listed) |
| Memory | 128 GB unified, per vendor spec | 7.9 GiB |
| OS / kernel | not recorded | Ubuntu 24.04.5 LTS under WSL2, Linux 6.18.40.1-microsoft-standard-WSL2 |
| Toolchain | not recorded | gcc 13.3.0, binutils 2.42, glibc 2.39, meson 1.3.2, Python 3.12.3, cargo 1.99.0 (rustup) |
| `/dev/kvm` | yes | no |
| Result | 142 passed / 36 skipped (libos), 28 passed / 8 skipped (pal) | identical counts, 0 failed; all example suites pass (helloworld, multi-thread, bash, busybox, redis, memcached, sqlite, python, lighttpd, nginx, rust, networks) |

Second-box notes:
- Needed `topo_info.c` to tolerate a missing `cache/indexN/number_of_sets` sysfs file (WSL2
  kernel omits it; same treatment as `physical_line_partition`, finding #4).
- Host packages beyond the base set: `bison`, `gawk`, `libevent-dev`, `sqlite3`, `automake`,
  `autoconf`, plus user-level pip `tomli`, `tomli_w`, `numpy`, `scipy` and rustup.
- Only the direct PAL was exercised. Nothing here covers the `vm` PAL (Milestone B), which
  needs QEMU `virt` and was not run; without `/dev/kvm` it would run under TCG.

## End goal and the two-backend decision

The actual target is **`gramine-vm` (the bare-metal unikernel PAL) working on arm64** —
not the `linux` PAL. Milestone A was defined as porting the LibOS/PAL arch layer using the
much-smaller `linux` backend as a validation vehicle, because it exercises the ~1,550 LOC
of LibOS-level arch code (syscall table, context/signal handling, TLS) that `vm`-PAL needs
too, without also requiring the ~16,600 LOC of new kernel-level subsystems (GICv3, PSCI,
ECAM PCI, VMSAv8-64 paging, SVE context switch, SVC-based syscalls) that the unikernel
needs on top. Everything built in Phases 1–4 (`common/arch/aarch64`, `libos/arch/aarch64`
— syscall table, TCB/context struct, TLS via `TPIDR_EL0`) is shared by both PAL backends
by design, specifically so it carries forward into Milestone B rather than being
throwaway work.

Tirith's `gramine-tdx/` is a fork of Gramine that only built/ran on x86_64. The port was
developed natively on an aarch64 Linux box with KVM available but no Intel TDX/SGX and no
ARM CCA/Realm support. Three scope decisions were made up front:

- **No TDX/CCA.** TDX is Intel-only silicon; this box has no ARM Realm support (`rme`
  feature absent from `/proc/cpuinfo`). The port targets a plain, non-confidential setup.
- **No graphics forwarding.** The `sg-listener`/`egl-redirect` graphics path is written
  against Intel i915 ioctls and is a separate redesign problem (this box's GPU is
  NVIDIA) orthogonal to CPU architecture.
- **Target the `linux` PAL first.** Gramine has two very different candidate "PAL
  kernels": `pal/src/host/vm/` + `vm-common/` is a from-scratch bare-metal x86 unikernel
  (own bootloader, GDT/IDT, APIC/IOAPIC, ACPI MADT, PCI via port I/O, 4-level page
  tables, XSAVE, SYSCALL/SYSRET MSRs — **~16,600 LOC**, entirely x86-specific,
  hard-gated to x86_64 in `meson.build`); `pal/src/host/linux/` + `linux-common/` is the
  ordinary backend where Gramine runs as a **regular sandboxed Linux process** (seccomp +
  signal handlers) whose x86_64-specific surface is only **~320 LOC** on top of the
  ~1,550 LOC of unavoidable LibOS-level arch code. The `linux` PAL validates the entire
  LibOS arch port with 50× less code and zero new kernel-level subsystems.

Goal achieved: `gramine-tdx` builds natively for aarch64 with
`-Ddirect=enabled -Dvm=disabled -Dsgx=disabled -Dtdx=disabled`, and the whole LibOS
regression suite passes under the `linux`/`direct` PAL on the box.

## What was built (the port phases, all complete)

### Step 0 — Branch

`arm64-port` off `main`, pushed to the original private repository. **Done.**

### Phase 1 — Shared `common/` arch layer

Smallest, most foundational layer; both PAL and LibOS depend on it. **Done.**

- Added `common/src/arch/aarch64/` and `common/include/arch/aarch64/` as siblings of the
  x86_64 dirs.
- x86 intrinsics replaced with aarch64 equivalents or dropped: `cpuid()`/`rdrand()`/
  `rdfsbase()`/`wrfsbase()`/`wrmsr()`/`rdmsr()` → `mrs`-based ID-register reads where
  needed; `get_tsc()` → `mrs cntvct_el0`; `flush_tlb()`/`invlpg()` → `TLBI VAE1IS` +
  `DSB ISH`/`ISB`; `cli()`/`sti()` → `msr daifset/daifclr`; `MB()/RMB()/WMB()` →
  `dmb`/`dsb` variants; `CPU_RELAX()` → `yield`.
- The x86 in/out-instruction-decode helpers (SGX/VM port-I/O emulation) are meaningless
  on aarch64 and omitted (only reachable from out-of-scope backends anyway).
- `.nasm` files (`ct_memequal.nasm`, `no_exec_stack.nasm`) became GAS `.S` — NASM doesn't
  target ARM at all; `meson.build` gates NASM to x86_64.

### Phase 2 — PAL arch layer (`linux` backend only)

**Done**, with one architectural correction made during the work: PAL_TCB access went
through several iterations (see findings below) before landing on a tagged-`TPIDR_EL0`
scheme plus a TLS-keyed registry, not the plain-global-only design originally sketched.

- `pal/src/arch/aarch64/pal_cpuid.c`: CPUID-leaf decoding replaced with `MIDR_EL1` reads
  (implementer/variant/part/revision fields mapped onto vendor/brand/family/model/
  stepping — the same mapping Linux's own `arch/arm64/kernel/cpuinfo.c` does).
- `pal/include/arch/aarch64/pal_arch.h`: `pal_get_tcb()` reads `TPIDR_EL0` via `mrs`,
  tagged with bit 0 to disambiguate "this thread's own PAL_TCB" from "the application has
  taken `TPIDR_EL0` over for its own libc TLS"; `struct PAL_CONTEXT` = `x0..x30`, `sp`,
  `pc`, `pstate`, `fault_address`, plus `orig_x0` (finding #6); `PAL_FPSTATE` =
  `fpsimd_context` (32×128-bit V regs + FPSR/FPCR); `ALL_SYSCALL_ARGS` = `x0..x5` (nr in
  `x8`); `union pal_csgsfs` dropped entirely (no segment registers).
- `pal/include/arch/aarch64/linux/{pal_host_arch.h,sigcontext.h,ucontext.h,sigreturn.h,sigset.h}`
  mirror Linux's real aarch64 extensible-record `sigcontext`/`ucontext_t` layout.
  `pal_set_tcb()` is a direct `msr tpidr_el0, x0` — the `arch_prctl(ARCH_SET_GS,...)`
  syscall doesn't exist on aarch64. `ucontext_revert_syscall()` checks `pc -= 4` against
  the `svc #0` encoding `0xd4000001` (vs x86's `rip -= 2` + `0f 05`).
- `pal/src/host/linux/arch/aarch64/{pal_arch_misc.c,pal_arch_exception.c,pal_start.S}`:
  `_PalSegmentBaseGet/Set` → direct `mrs/msr tpidr_el0`; `pal_start` entry uses `x0`/`x1`.
  `pal_arch_exception.c` also hosts the PAL_TCB registry (findings #9, #12, #13).
- `pal/src/host/linux-common/arch/aarch64/syscall.S`: `do_syscall` is *simpler* than x86
  (AAPCS64 arg registers already match the syscall convention — no shuffles);
  **`__NR_vfork` doesn't exist on aarch64** — emulated via
  `clone(CLONE_VM|CLONE_VFORK|SIGCHLD)`; `syscall_rt_sigreturn` and
  `_PalThreadExit_asm_stub` rewritten.
- `stack_probe` GAS `.S`; `pal.lds` → `OUTPUT_ARCH(aarch64)`;
  `pal/include/arch/aarch64/pal_topology.h` queries huge-page sizes at runtime instead of
  hardcoding x86's 2M/1G.

### Phase 3 — LibOS arch layer

**Done.** The largest and highest-risk phase; unavoidable regardless of PAL backend.

- **`libos/src/arch/aarch64/libos_table.c`**: new syscall-number-indexed dispatch table
  against aarch64's **generic syscall ABI** (`asm-generic/unistd.h`). x86-only entries map
  to their `*at`/`ppoll`/`dup3`/`clone`/`pipe2`/`getdents64` successors — Gramine's
  C-level handlers were already written against the modern calls, so this was largely
  "point the table at what already exists". New
  `libos/include/arch/aarch64/linux_abi/syscalls_nr_arch.h`; Gramine's custom syscall
  numbers (`__NR_ACQUIRE_LLOCK` etc.) placed consistently outside the real range.
- **`libos/src/arch/aarch64/libos_context.c`**: redesign, not line-port. FP/SIMD via
  `fpsimd_context` (+ optional `sve_context`, VL via `PR_SVE_GET_VL`, not CPUID);
  clone-child return `x0 = 0`; `prepare_sigframe()` 3-arg convention in `x0/x1/x2`;
  `restart_syscall()` via `x8`/`pc`; `maybe_emulate_syscall()` detects the 4-byte `svc`
  encoding.
- **`libos/src/arch/aarch64/syscallas.S`**: full redesign. Gramine's LibOS intercepts
  syscalls in **userspace** (seccomp trap or patched-libc indirect jump), not via hardware
  trap semantics; the trampoline is built on a plain AAPCS64 function-call contract. Now
  also carries the full-table PAL_TCB registry scan (finding #13) and the X16 resume
  contract (finding #1).
- **`libos/src/arch/aarch64/start.S`**, **`libos_elf_entry.S`** (GAS: `MXCSR` setup
  becomes FPCR/FPSR reset), **`libos_tcb_arch.h`** (`TPIDR_EL0`-relative access),
  **`libos_cpuid.{h,c}`** (hwcap strings instead of CPUID leaf tables),
  **`fs_proc/info.c`** (real aarch64 `/proc/cpuinfo` format: `CPU implementer`/`part`/…),
  **`gramine_entry_api.h`** (`GRAMINE_SYSCALL` as a `TPIDR_EL0`-based indirect branch;
  forward-compat only — no patched guest libc exists yet).
- Reused as-is after verification: `linux_abi/types_arch.h`, `linux_abi/errors_arch.h`
  (errno table identical on both arches), `libos_internal_arch.h` (`EM_AARCH64`),
  `libos_vdso_arch.h`.
- One real semantic difference carried through: `RED_ZONE_SIZE` = **0** (AAPCS64 has no
  red zone; x86_64 SysV has 128).

### Phase 4 — vDSO arch layer

**Done.**

- `vdso_arch_syscall()` rewritten around `svc #0` (nr in `x8`) — vdso.so links with
  `--no-undefined` and never reaches the PAL's TLS machinery.
- `vdso.lds` NOP-padding: x86 1-byte `0x90909090` fill → aarch64 4-byte NOP
  `0xd503201f` (fixed instruction width changes the fill mechanics).
- `vdso.c` logic and `vdso_note.S` were already arch-agnostic.

### Phase 5 — Build system wiring

**Done.** `arch/aarch64/` sibling dirs added everywhere `arch/x86_64/` exists — the
`subdir(host_machine.cpu_family())` dispatch pattern needed zero changes. The
`-mfxsr -mxsave` and NASM gates in meson already keyed off `x86_64` and fall through
cleanly. Build config: `meson setup build-aarch64/ --buildtype=release
-Ddirect=enabled -Dsgx=disabled -Dvm=disabled -Dtdx=disabled -Dtests=enabled
-Dlibc=glibc` — a **native build**, no cross-file. `-Dlibc=glibc` chosen because the
musl *test-binary* variant needs a system `musl-gcc` package requiring sudo; Gramine's
own bundled musl runtime build is unaffected.

### Phase 6 — Build, fix, and validate

**Done.** Clean build; then the finding-by-finding fix chain below drove the suite from
4 passing to fully green.

## Findings (the blocker chain, all resolved)

Suite progression across the sessions: **4 → 50 → 64 → 93 → 100 → 132 → 140 passed**
(was 4/137 at first run; full-suite wall time dropped from 2m19s to ~21s once the
threading crashes stopped crash-spinning).

### Bootstrap batch (PAL self-relocation and early start, all fixed)

Fixed in the first bring-up session, in order of discovery:

1. **PAL_TCB unreachable in `libpal.so`'s own bootstrap.** `libpal.so` is a
   self-executing, no-`PT_INTERP` ET_DYN binary (no real dynamic linker processes its
   relocations) — but `g_pal_tcb_singleton` had default visibility, so the compiler
   emitted GOT-indirect loads even from within `libpal.so`, and that GOT entry is never
   populated. Fixed with `protected` visibility (still exported for LibOS's cross-object
   GOT access, but direct relocation-free addressing from inside `libpal.so`) and
   hand-written `pal_get_tcb()`/`pal_set_tcb()` choosing direct vs GOT-indirect addressing
   via `IN_PAL`.
2. **`pal_rtld.c`'s `perform_relocations()` only knew x86_64 relocation types.** Added
   `R_AARCH64_RELATIVE`/`GLOB_DAT`/`JUMP_SLOT`/`ABS64` plus an arch-conditional
   `RELOC_TYPE_*` indirection.
3. **Stack-protector canary read during self-relocation.** aarch64 falls back to the
   generic `__stack_chk_guard` scheme (no register-relative option like x86_64's
   `%gs:0`), read in every protected function's prologue — including
   `perform_relocations()` itself, before the GOT exists. Fixed with
   `#pragma GCC optimize("no-stack-protector")` for the whole of `pal_rtld.c` (the only
   code that ever runs pre-relocation).
4. **`topo_info.c` treated a missing `physical_line_partition` sysfs file as fatal**
   though the kernel omits it for some cache indices. Default 0 on `-ENOENT`, matching the
   tolerance already used for other optional attributes in the same file.
5. **`clone()`'s stack adjustment broke AArch64's mandatory 16-byte SP alignment.**
   `syscall.S` reserved `0x18` (24) bytes for `f`/`arg`/`exit_func`; the child's first
   `ldr x9, [sp]` took an SP-alignment fault (SIGBUS). Reserve `0x20`.
6. **`illegal_upcall()` misattributed ordinary guest syscalls as fatal internal
   corruption.** For SIGSYS, `handle_sync_signal()` passed `info->si_addr` as the faulting
   address — but the kernel doesn't populate `si_addr` for SIGSYS (it supplies
   `si_call_addr`). The bogus address made `lookup_vma()` fail and take the "else fatal"
   branch for completely normal guest syscalls. Architecture-generic bug in shared
   `pal_exception.c`, only ever exercised on an arch that routes *all* syscalls through
   the seccomp-trap path. Fixed by using the already-reverted `rip`.
7. **Gramine's own IPC worker thread broke the single-threaded PAL_TCB stopgap** the
   instant LibOS init started it (independent of guest threading): one process-wide
   global, and the worker's `pal_set_tcb()` overwrote it, corrupting the main thread's
   `get_cur_thread()` forever. Fixed with the **tagged-`TPIDR_EL0` scheme**: each thread's
   architecturally per-thread `TPIDR_EL0` holds its PAL_TCB pointer with bit 0 tagged;
   `pal_get_tcb()` checks the tag first and falls back to `g_pal_tcb_singleton` only when
   untagged (i.e. the application's libc owns TLS); the one thread that needs the
   fallback gets it snapshotted via `pal_tcb_singleton_snapshot_before_user_entry()` right
   before `TPIDR_EL0` is handed to application TLS at both process start and `execve()`.

### Finding #1 — the seccomp-trap path clobbering the application's live X30

There *was* an infinite loop ~9 syscalls into `ld.so` startup — a 3-instruction
userspace loop inside ld.so's frameless `munmap` wrapper (`cmn → b.cs → ret`, where `ret`
jumped back to the `cmn`). Instrumented round-trip tracing showed exactly 9 correct
emulation round-trips and then the spin at `'ret' with x30 = svc+4`.

**Root cause:** the emulation path carried the *resume address* in the guest context's
**X30** (mirroring x86_64's `%rcx`). x86_64 can do this safely because the hardware
`syscall` instruction itself destroys `%rcx` — no application can hold live state there
at a syscall site. aarch64's `svc` destroys **nothing**: X30 at an `svc` site is the
application's own **live link register**. Installing the resume address over it corrupts
every leaf-style libc wrapper that `ret`s via X30 right after the `svc` — the first 9
syscalls survived only because their glibc call sites reload the true X30 from their
stack frame, masking the clobber until the first frameless wrapper.

**Fix — new entry contract, resume address in X16** (IP0/IP1 are architecturally
sanctioned clobber registers, already declared sacrificed by `syscallas.S`):
`maybe_emulate_syscall()` sets `context->regs[16]`, leaving `regs[30]` as plain app state;
`libos_syscall_entry` treats "X16 = resume address" as the entry contract and mirrors it
into `context->regs[16]` (the restart invariant); `_return_from_syscall` restores X16 and
branches via X17; the `GRAMINE_SYSCALL` macro became `adr x16, 1f; <chain via x17>; br
x17`, matching real `svc` clobber semantics. (It was previously the *only* correct path,
which is why `GRAMINE_SYSCALL`-based tests passed while every real-libc test spun.)

**Also fixed:** a leftover `log_always("A quick check.")` in `_PalSystemTimeQuery()`
(initial-Tirith-commit debug spew, also affecting x86) firing on every clock query.

**Performance takeaway:** the earlier "one syscall every several seconds" reading was
entirely a GDB/ptrace artifact; untraced, full `ld.so` + `helloworld` startup through the
SIGSYS round-trip path takes ~10ms wall. The seccomp-trap-per-syscall design may still
cost throughput on syscall-heavy workloads (the patched-libc fast path remains the
eventual answer, per Deferred), but there is no anomaly left to hunt.

### Finding #2 — the aarch64 glibc runtime was missing its loader (packaging, not port code)

First full-suite run after the X30 fix: **50/91/35**. The bulk of failures were one
harness-level cause: `find_interp()` could not resolve `PT_INTERP` (`/lib/ld-linux-aarch64.so.1`)
inside the manifest's `/lib` chroot — `glibc_output_paths` in the glibc subproject
meson.build listed the loader only for x86_64, so the installed aarch64 runtime dir had
libc/libm/etc. but **no `ld-*` at all**. Only the ~28 manifests that additionally mount
the host `/lib/aarch64-linux-gnu` escaped — which is why `helloworld` passed while almost
nothing else did. Fixed by adding the aarch64 branch (`elf/ld-linux-aarch64.so.1`,
`elf/ld.so`, `mathvec/libmvec.so{,.1}`). `compile.sh` needed no changes — the loader
binary was built, just never installed.

### Finding #3 — aarch64 `clone()` raw-ABI argument slots: every guest `fork()` EFAULT

Every `fork()`-using test failed with `fork(): Bad address`. Diagnosis chain worth
recording (each tier contributed a distinct fact): strace showed a non-NULL
`child_tidptr`; Gramine's own debug log showed the CLONE_SETTLS-unset path followed by
EFAULT (only possible if `child_tidptr` read as **0**); raw sigcontext dump inside the
PAL's SIGSYS handler showed `x4 = the TPIDR_EL0-relative tcb field`; glibc disassembly
settled it: `_Fork` hand-places **ctid→x4, newtls→x3**.

**Root cause:** `libos_syscall_clone(flags, stack, parent_tidptr, child_tidptr, tls)` is
written to x86_64's slot order and the aarch64 table calls it positionally — but on
aarch64 the guest libc puts **TLS in x3 and child_tidptr in x4**, swapped. Every `fork()`
read ctid=x3=0 → EFAULT; every `pthread_create` silently used the *ctid pointer* as its
TLS base. **Fix:** `libos_syscall_clone_raw()` registered at `[__NR_clone]`, tail-calling
the unchanged handler with x3/x4 exchanged, plus NULL-`child_tidptr` tolerance under
`CLONE_CHILD_SETTID` as kernel-alike hardening.

### Finding #4 — forked child crashed in the `pal_get_tcb()` singleton after `set_tls()`

Every forking test *hung* with the child in a hot `SIGSEGV`/`rt_sigreturn` loop (fault
`0x10`) in `rs_thread()` right after its `msr tpidr_el0`. `rs_thread()` is the first
point in a forked child where `TPIDR_EL0` stops holding the tagged PAL_TCB; the untagged
fallback resolved the *checkpoint-restored* singleton slot, which pointed into the
parent's PAL_TCB — an anonymous region the checkpoint blob restores as fresh zero pages
in the child (pointers survive, contents don't). **Fix:** snapshot the singleton from the
child's own still-tagged `TPIDR_EL0` immediately before `set_tls()`. `pipe` became the
first test to pass fork end-to-end on aarch64. (Debugging note: on aarch64 `adrp`
resolves relative to the *PC page* and libsysdb's `.text` maps ~0xa1KB above its load
base — GOT addresses must be computed from the faulting PC's page, not the ELF base.)

### Finding #5 — signal handlers had no restorer on aarch64

Handlers ran but the process died at the handler's `ret` — LR was 0. Gramine's
`prepare_sigframe()` set the handler's return address from the caller-provided
`sa_restorer`, the x86 mechanism (glibc's x86 `sigaction()` injects `SA_RESTORER` + a
libc stub). aarch64 glibc passes *no* restorer: on arm64 Linux the **kernel** writes
`mov x8, #139; svc #0` into the signal frame and points LR at it. The port inherited only
the SA_RESTORER half. **Fix:** aarch64 `prepare_sigframe()` ignores the restorer argument
(the arm64 kernel ignores `SA_RESTORER` too) and points LR at a two-instruction
trampoline in libsysdb's own executable .text — deliberately *not* in the frame (guest
stacks are non-executable; making them executable per signal would be a security
regression since only SP matters to `rt_sigreturn`). `illegal_upcall()`'s
"internal execution" check whitelists exactly that one site (PC in the 16-byte window
*and* the trapped word is `0xd4000001`).

### Finding #6 — restarted syscalls ran with arg0 clobbered by the `-ERESTART*` code

On aarch64 **x0 is both arg0 and the return register**. `do_syscall()` writes the return
value — including `-ERESTARTSYS`/`-ERESTARTNOHAND`/`-ERESTARTNOINTR` — into x0 *before*
`restart_syscall()` re-dispatches: `wait4(-1)` restarted as `wait4(-512)`, `read(fd)`
restarted with `fd = -ERESTARTSYS` → EBADF. x86_64 is immune (`%rax` ≠ `%rdi`). **Fix:**
mirror the arm64 kernel's own remedy (`pt_regs->orig_x0` exists for exactly this reason):
new `PAL_CONTEXT.orig_x0` snapshotted in `do_syscall()` before dispatch, restored by
`restart_syscall()`. The unconditional write *must* stay — `handle_signal()`'s
`-ERESTART*` classification reads it back via `pal_context_get_retval()`.

### Finding #7 — Tirith's "secure gaming" host-redirection wrote a never-mapped comm page

Eight tests (`shm`, `close_range`, `devfs`, …) died identically at `Internal memory fault
at 0x00f00018`. Tirith's SG code (`#define ENABLE_SG`) redirects matching `/dev`//`/sys`
`openat`s into `COMM_ADDR = 0xf00000` — a fixed host-shared page only the full
VM/TDX+sg-listener deployment maps; the plain Linux PAL has no host agent, and
`is_sg_host_path()` matches *every* `/dev`/`/sys` path wholesale. The design's own safety
valve (`magic` check) can't help: every call site writes `c->p1..p3` before calling it.
Fixed at the single place `TYPE_HOST` handles are created, gated on
`g_pal_public_state->host_type` being VM/TDX — the runtime dispatch the codebase already
uses elsewhere.

### Finding #8 — AF_INET UDP: VM-PAL shared-ring assumptions broke `socket()` and `recvmsg`

`socket(AF_INET, SOCK_DGRAM)` → ENOSYS: `libos_syscall_socket()` eagerly mmaps the VM-PAL
UDP receive ring and a failed `register_shmem()` was treated as fatal, though the ring is
optional. `-ENOSYS` is a capability report — drop the region, continue ringless. Then
`recvmsg` returned full datagram length: the SOCK_DGRAM fast path returns the PAL's
length verbatim (the PAL forces `MSG_TRUNC` on the host recvmsg for ring transports) and
skipped the POSIX clamp. Applied the clamp (return bytes-copied unless the *caller* asked
for truncation; set the MSG_TRUNC output flag when the datagram didn't fit).

### Finding #9 — per-thread PAL_TCB registry: the entire multi-threading cluster

~17 threaded tests (eventfd×3, multi_pthread, fp_multithread, signal_multithread,
futex×3, spinlock/rwlock, OpenMP, join hangs, exit_group) shared one root cause: every
thread's `pal_get_tcb()` fell back to `g_pal_tcb_singleton`, so *all* guest threads
shared one PAL_TCB — one shared LibOS emulation stack (the `libos_syscall_entry` prologue
switches SP to `tcb->libos_tcb.libos_stack_bottom`), one `libos_tcb_t`, one canary
region. Forensic fingerprint: `__stack_chk_fail` firing in random functions, returns into
garbage, joins that never woke.

Fix without owning TPIDR_EL0 (impossible: `msr tpidr_el0` is unprivileged so libc's own
TLS setup can't be intercepted, and the glibc aarch64 TCB region has no spare word): a
128-entry registry mapping each thread's **application TLS pointer** — per-thread unique,
and exactly what `set_tls()` installs, a child's first act before any syscall can trap —
to its own PAL_TCB. Registration hooks into `set_tls()` itself; a scan miss can only be
the main thread (libc plants its TLS with no Gramine hook), for which the singleton
fallback *is* correct, so `pal_get_tcb()` lazily self-registers it. (Two claims in the
original commit — "no deregistration needed" and "scan stops at first empty slot" — were
later superseded by findings #12/#13.) The asm scan clobbers only ABI-sacrificed
x16/x17/x8, parked in one 16-byte frame and restored verbatim.

### Finding #10 — exec entry handoff must zero x0: glibc registered `_start` as its own `rtld_fini`

`basic_bootstrapping_static` crashed *during exit()* after printing everything. The
kernel's `execve` enters a new program with **all GPRs zero**, and on aarch64 this is
load-bearing: glibc's `_start` begins with `mov x5, x0` — incoming x0 *is* the
`rtld_fini` argument to `__libc_start_main`. The port jumped with x0 = the entry address
(it doubles as the `br` target), so glibc registered `_start` as the atexit handler and
**re-ran `__libc_start_main`** on exit. **Fix:** stage jump targets in x16/x17 and zero
x0..x8 before `br x16`, plus zero NZCV/FPCR. The aarch64 sibling of finding #1: the entry
contract must match what a real kernel delivers, register for register.

### Finding #11 — SG stat-path gates + two small semantics gaps (the pseudo-FS backlog)

1. Finding #7's gate had to cover **stat** paths too — Tirith's SG code also redirects
   `stat`/`newfstatat` matches on `/dev`//`/sys`//`/proc/sys` into the never-mapped
   window; both stat-side predicates now consult the same `sg_redirect_enabled()` helper.
2. `libos_cpuid.c`'s auxv read passed `PAL_CREATE_IGNORED` where `PAL_CREATE_NEVER` is
   meant (the linux PAL `BUG()`s on IGNORED for regular-file opens; NEVER is exactly
   "open only because it already exists").
3. `unlinkat(AT_REMOVEDIR)` lacked x86's `rmdir()` mount-point protection — on aarch64
   glibc's `rmdir()` *is* `unlinkat(AT_REMOVEDIR)`, and Gramine's synthetic-fs branch
   silently deleted the mount point the `synthetic` test verifies is unremovable. Added
   the matching `-EACCES`.

Test-side in the same batch: `syscall.c`'s raw-syscall helper gained the aarch64 flavor
(bare `svc #0`, nr in x8 — exactly what an un-patched libc wrapper executes under trap
emulation), plus x86-only expectation guards and an architecture skip for the
`asm/x86_64` module.

### Finding #12 — stale TLS-keyed registry bindings after exec sweeps; registry moves to libpal

`exec_same` (execve to the same binary in a loop, with a second pthread) crashed
intermittently with a *live* thread acting on a *freed* `libos_thread` struct. Root
cause: the exec sweep munmaps the old address space while a dying thread is still
resolving itself; the new image's stacks **routinely re-mmap at the very same addresses**
(deterministic allocator reuse), so a new winner's scan hit the dying thread's stale
`[old_tp → old_TCB]` binding and ran its emulation *as the other thread*. Fixes:

- **The registry moved from libsysdb to libpal.so** (one shared table + lock for both
  DSOs, exported via `pal_symbols`). Pitfall: `visibility("protected")` on a function
  *declaration* makes GCC emit non-PLT calls the linker refuses to bind across shared
  objects — the attribute belongs only on *definitions*, plain prototypes in the header.
- **Ownership-checked deletion**, `libos_tcb_registry_del(tp, expected_tcb)`: a key is
  just a TLS address, legitimately *rebound* by a new thread born from mmap reuse — a
  plain key-match deletion steals the new owner's registration, and that live thread then
  runs its whole life as the main thread (this exact ABA caused the
  `rwlock`/`gettimeofday`/`futex_*` wake-queue corruption family).
- **Deregistration at every abandon point** — `thread_exit` entry, the exec winner before
  `set_default_tls()`, and `_PalThreadExit` before its asm stub — always **resolving
  one's own PAL_TCB before deregistering**, then immediately switching TPIDR_EL0 to the
  tagged own-TCB (order reversed = an exiting thread resolving the singleton and deleting
  the main thread's live list node — observed verbatim).
- **Exit claim** (`libos_thread.exiting` CAS in `handle_signal`): the emulated-svc
  epilogue calls `handle_signal()` after *every* completed syscall — including a futex
  wait interrupted by the exec winner's `tgkill(SIGCONT)` — so a marked thread's exit can
  be entered through the signal upcall; without the claim both entrants run `thread_exit`
  and the second double-frees.
- **Stranger guard** at `thread_exit` entry: a thread whose list node is already removed
  (mis-resolution through a stale binding — deregistration shrinks but cannot fully close
  the window between list removal and deregistration) just exits its host thread with
  `PalThreadExit(NULL)`.

### Finding #13 — the asm TCB scan's "stop at first empty slot" died the moment any deregistration existed

After finding #12, *unrelated* single-process wake-queue tests began crashing with a
bewildering menagerie (canary failures with LRs into the raw-syscall region,
`move_to_wake_queue` faults at `[NULL+0x10]`, fresh `clone` children reading `[sp]` slots
as zero and jumping to PC=0). A four-build bisection matrix (entry-deregistration ×
`_PalThreadExit`-deregistration on/off) plus a live gdb dump of the registry at crash
time localized it: **the scan stopped at the first empty slot** — valid only while the
table was append-only, but every deregistration frees a slot at an arbitrary index, so a
hole *below* a thread's own entry truncated its scan → phantom miss → **singleton
fallback → that thread runs its entire life on the MAIN thread's TCB**. All the symptoms
are downstream fingerprints of two threads sharing one TCB. Fixes: **full 128-slot scan**
terminated by `tst x17, #2047 == 0` (legal because the table is `aligned(2048)` —
enforced by `static_assert` alongside the stride/offset asserts), `{key, NULL}`
add/del-transients skipped not hijacked (TLS keys are unique per live thread),
ownership-checked del everywhere, deregister-after-resolve ordering. Verified: the five
wake-queue tests 15× each, `exec_same` 15× idle + 8× under 12-core load, rwlock 3× under
load, full suite 140/0/36.

Debugging notes: (a) a `ra0=%p` probe in `__stack_chk_fail` was *misleading* — when the
canary check fails the saved LR is usually smashed with it, so a smash's "caller" cannot
be trusted; the wake-queue state and per-thread TCB identity are what discriminate.
(b) `g_pal_tcb_registry`/`g_pal_tcb_singleton` are fixed-address symbols in libpal —
`gdb -ex 'x/130xg &g_pal_tcb_registry'` at crash time, cross-checked against `info
threads`' per-LWP tpidrs, immediately shows missing/duplicate/singleton-hijacked
bindings. (c) Never `log_always` from inside registry add/scan/del: `log_always` →
`pal_get_tcb()` → scan/add → recursion → stack smash.

### Finding #14 — SG fd-number paths in close()/lseek()/getdents64() faulted on the unmapped comm window

Found by running the iperf3 example from `gramineproject/examples`: the server died right
after its first test with `Internal memory fault at 0x00f00018`. iperf3 `close()`s an fd that
has no LibOS handle; Tirith's `ENABLE_SG` code treats any handle-less fd as a host (SG) fd
and writes the request into the comm window at `COMM_ADDR = 0xf00000`, which only the VM/TDX
deployment maps (same family as finding #7). Finding #7 gated the sites keyed on
`hdl->type == TYPE_HOST` (inert without SG handles) but three sites key on the raw fd number
and stayed live: `close()` with no handle, `lseek()` (which even shifts an invalid fd by
`fs_offset = 500` into the host range) and `getdents64(fd > 500)`. All three now require
`sg_redirect_enabled()`; without a host agent they return `-EBADF`. Regression test:
`bad_fd` (`test_207_bad_fd`), which reproduces the exact fault on the old code. Pre-existing
(reproduces with `sys.rewrite_syscalls = false`).

### Registry invariants (violating any resurrects the singleton-hijack family)

- Full-scan termination requires `sizeof(g_pal_tcb_registry) == 2048` (`aligned(2048)`
  + `static_assert`).
- Deletion is ownership-checked (`del(tp, expected_tcb)`).
- Every abandon point resolves its own PAL_TCB **before** deregistering, then tags
  TPIDR_EL0 (`pal_get_tcb() | 1`).
- LIST_EMPTY stranger guard at `thread_exit` entry; `exiting` claim CAS in
  `handle_signal`.
- Never log from the registry hot path.

## Current state

**`gramine-direct helloworld` prints `Hello world!` and exits 0 in ~10ms.** Full scorecard
and validation below. The fork cluster, signal cluster and **multi-threading cluster** are
fully functional: `pipe`, `fork`/`wait`/`exec`/zombie/`double_fork`, `syscall_restart`
(both flavors), `itimer`, `sigaltstack`, `sigprocmask_pending`, `sighandler_reset`,
`multi_pthread` (256 threads), `close_range`, `flock_lock`, all `eventfd` flavors,
`fp_multithread`, `signal_multithread`, `pthread_set_get_affinity`, spinlock/rwlock
unittests, the pseudo-FS quintet, `basic_bootstrapping_static`, `syscall_redirect`,
`stat_invalid_args`, and the whole `exec_same` (fork+exec × 50-args) family. The only
test not executed is the x86-only `iret` emulation test (skip-guarded by architecture;
SGX-only, no aarch64 analog).

### Remaining triage backlog

- **Known intermittent hang (found 2026-10-08 by `scripts/stress_rewrite.sh`, not yet
  root-caused):** a process that makes syscalls in a loop while receiving asynchronous signals
  (itimer SIGALRM at 200 us and/or `pthread_kill` from another thread every ~30 us, handlers
  doing syscalls) occasionally livelocks: the main thread sits at 100% user CPU forever and
  `SIGTERM`/`timeout` do not stop it (needs SIGKILL). Rate on the WSL2 box with the pure trap
  path (`sys.rewrite_syscalls = false`): ~2.7% of cold runs of `rewrite_stress signals`
  (4/150), ~0.8% timer-only (1/120), ~3% kill-only (4/120); with rewriting on it is the same
  order (1/60). Native never hangs (0/200). It disappears under `log_level = "debug"`
  (heisenbug; 80/80 clean). Reproduce: `scripts/stress_rewrite.sh -n 150 --rewrite off
  signals_kill`. Prime suspect: a non-reentrant spinlock held by LibOS/PAL code when a
  signal handler upcall runs on the same thread (user-mode spin, no futex wait). Needs gdb or
  lock-ownership instrumentation.
- Otherwise nothing known-failing: all entries of every previous backlog are closed (findings
  #7–#13).
- Known *latent* races to keep in mind for Milestone B: the `add()` key-match **rebind**
  window (bounded and harmless today because the dying thread deregistered-and-tagged at
  `thread_exit` entry before anything can rebind; the LIST_EMPTY stranger guard is the
  backstop), and the per-generation determinism of mmap reuse (any future change that
  re-opens a deregistration window will silently resurrect the singleton-hijack family —
  the full-table-scan + ownership-del + static_assert triangle is what makes that family
  loud instead of silent).
- `libos/test/regression/root/` is an untracked test-build artifact (chroot target for
  `tmp_enc` mounts) — leave untracked.
- Debugging notes that stay useful: on aarch64, `adrp` targets are PC-page-relative and
  libsysdb maps `.text` ~0xa1KB above its load base — GOT-slot runtime addresses must be
  computed from the *faulting PC's page*; symbolize against the *currently installed*
  binary (every diagnostic edit shifts static offsets); the libsysdb runtime base is
  recoverable from any libsysdb-side log carrying the address of a known static.
- GDB interplay notes for the seccomp-trap path: breakpoints in `libpal.so`/`libsysdb.so`
  need hand-computed `runtime_base + static_offset` (no `r_debug` hook for GDB's
  pending-breakpoint-on-load); attaching GDB to a spinning/seccomp-trapped process can
  *mask* the spin (ptrace signal interplay) — instrumented tracing localized finding #1
  where gdb could not.

### Deferred (explicitly, not forgotten)

- ~~Real per-thread PAL_TCB for a multi-threaded guest application.~~ **DONE** — finding
  #9. The musl-`struct pthread`-anchored scheme originally sketched turned out
  unnecessary: no libc patching needed once the key is the application TLS pointer
  itself. Gramine's own internal threads still use the tagged-`TPIDR_EL0` fast path (no
  application TLS to anchor to); the main thread self-registers lazily on its first scan
  miss.
- **Patched-glibc (`GRAMINE_SYSCALL`) fast path for aarch64.** All guest syscalls
  currently take the seccomp-trap round trip (~10ms for full `ld.so` startup; correct but
  syscall-heavy workloads pay per-trap overhead). The macro machinery exists and is
  aarch64-correct; what's missing is a Gramine-patched aarch64 libc. The vm-PAL will want
  this for performance (SVC trap is cheaper than SIGSYS but still a full round trip).

## Verification (all passing)

- `ninja -C build-aarch64/` completes with no errors.
- LibOS regression (`libos/test/regression`): **140 passed, 0 failed, 36 skipped** (36 =
  aarch64-irrelevant skips: musl-only, x86-asm, platform guards added along the way).
  Stress on top: `exec_same` (50 args) 15× idle + 8× under 12×`yes` load, `rwlock` 3×
  under load, the wake-queue quintet 15× each — all clean.
- PAL regression (`pal/regression`): **28 passed, 8 skipped** (8 = SGX/EDMM guards).
  `test_102_cpuinfo` was x86-only in its expectations; it now has an aarch64 branch
  validating the MIDR-based `_PalGetCPUInfo` emulation against the host's per-core
  `CPU implementer`/`part`/`variant`/`revision` fields — accepting any distinct core type,
  since on this heterogeneous (big.LITTLE) host `mrs midr_el1` reflects whichever core the
  process ran on and the full 16-hex MIDR is not reconstructable from `/proc/cpuinfo`.
- Re-verified green after removing the pre-existing Tirith debug leftover
  `log_always("Getting TLS from current thread")` from `libos_clone.c` (fired on every
  `fork()`; same category as the `A quick check.` removal).
- **`install.sh`** (repo root) automates the build/install chain: apt deps (opt-in
  `--deps`, prints the line when sudo isn't available), `meson setup` with the repo's
  tested options (direct on, sgx/vm/tdx off, tests on, glibc, private prefix
  `$HOME/.local/gramine`, build dir `build-$(uname -m)`), `ninja` + `ninja install`, and
  optional chaining into `run_all_tests.sh --no-build` via `--run-tests`. Idempotent.
- **`run_all_tests.sh`** (repo root) drives the whole battery — PAL regression, LibOS
  regression, helloworld, multi-thread, busybox, redis, memcached — with per-suite logs
  under `$LOGDIR`, bounded lifetimes, guaranteed stray cleanup, non-zero
  exit on any failure. Arch-neutral: also drives x86_64 parity runs (point
  `GRAMINE_PREFIX` at a native install; foreign-arch example binaries self-rebuild).
- x86-parity hygiene in CI-Examples: the multi-thread manifest templates hardcoded
  `x86_64-linux-gnu` in `LD_LIBRARY_PATH` (dead entries on aarch64); they now take a
  `{{ multiarch }}` parameter fed by `$(CC) -dumpmachine`. Byte-identical output on x86.

### Example applications (real-world validation)

Run under `gramine-direct` on the aarch64 host; all network exercise over real TCP
through the direct-PAL port stack.

| Example | Result | Notes |
|---|---|---|
| `helloworld` | **Pass** | trivial baseline |
| `multi-thread` (`thread-tls`, `thread-rwlock`, `thread-stress`) | **Pass** | exercises the per-thread PAL_TCB registry end-to-end |
| `redis` 6.0.5 | **Pass** | `Ready to accept connections`; `PING→PONG`, `SET→OK`, `GET→bar` over TCP — first real-world epoll+TCP server on the port |
| `busybox` 1.32.0 | **Pass** | `sh -c "echo; uname -m; uptime; ls /lib"` all work. Needed `CONFIG_TC`/`CONFIG_ARP`/`CONFIG_ARPING` disabled (upstream busybox ↔ Ubuntu-24.04 kernel-headers breakage, `TCA_CBQ_MAX`; unrelated to the port) |
| `memcached` 1.6.21 | **Pass** | 3 fresh-start cycles × 100 pipelined `set`/`get` transactions, every payload byte-checked; `incr` counter semantics exact. libevent 2.1.12 headers built locally into `deps/local` (no dev package, no sudo); runtime resolves the host's same-SONAME `libevent-2.1.so.7` through the manifest's existing `/usr/lib/aarch64-linux-gnu` mount — no manifest change needed |
| `align_test` | Dead example | sources fully commented out; the real version maps the SG comm region (`0xf00000`) — a VM-PAL/SG feature, out of scope |

Harness gotchas learned (not port bugs, but they mimic flakes):

- Example manifests pin `loader.argv` and reject command-line args (`PAL failed argv
  handling wasn't configured…` + instant exit) — extra args like `-vv`/`-m 64` kill the
  server before any traffic; run argument-free.
- `gramine-manifest` needs `PYTHONPATH` pointing at the installed `graminelibos`, else
  manifest generation fails with rc=127 leaving a **0-byte manifest**.
- Rapid kill/restart of a TCP server leaves the port in `TIME_WAIT`; memcached's *UDP*
  listener (same port) then logs `failed to listen on TCP port 11211: Address already in
  use` while its TCP listener binds fine via `SO_REUSEADDR` — benign spew if clients still
  get correct answers.
- Text-protocol test clients must declare the exact value byte count — `set k 0 60 3`
  with a 2-char value earns a *correct* `CLIENT_ERROR bad data chunk`; several apparent
  "server flakes" during bring-up were client framing bugs, resolved by a makefile-framed
  byte-count-checked client.
- Server readiness must be gated on a successful command round-trip (PING / `version`),
  not `connect()`: both servers `listen()` early in startup, so an accepted connection can
  briefly precede any ability to answer.

## Tests worth running later (not yet done)

Prioritized follow-ups; none block Milestone B, but (1)–(3) are cheap and high-value:

1. ~~**x86_64 parity run**~~ — **DONE** (2026-10-08, see `ARM64_X86_COMPARISON.md`):
   x86_64 **32/0/4 (PAL) + 144/0/32 (LibOS)** vs aarch64 **28/0/8 + 140/0/36**; identical
   test counts (36/176), every skip-set difference traced to an explicit arch/SGX guard,
   zero failures on both. Three x86-side fixes came out of it (`send_handle.c` memcmp
   over-read, `memory.c` arch-neutral helper placement, x86 `pal_arch.h` snapshot hook).
   Re-run the diff after any change to shared code.
2. **Repeat-determinism soak** — `run_all_tests.sh` back-to-back ×5 with zero failures,
   and the load-sensitive quintet + `exec_same` overnight loops. The crash family fixed in
   #12/#13 was intermittent (1-in-tens); a long soak is the only credible proof the
   registry invariants hold under scheduling variance.
3. **LTP suite** (`libos/test/ltp/`) — the largest untapped validator; collection
   currently fails because the LTP sources aren't fetched (build via its Makefile
   first). Its syscall coverage (hundreds of tests × fs/mm/net) far exceeds the
   regression suite. Expect arch-neutral failures to be genuine port bugs.
4. **More real-world apps**, same harness pattern as redis/memcached: `nginx`
   (epoll + accept-multiply + sendfile), `sqlite3` (heavy file I/O + `mmap` + advisory
   locking), `openssh`/`dropbear` (pty, signals, sockets), `python3` interpreter itself
   (huge dynamic surface). Each needs only a manifest + local build like memcached did.
5. **Seccomp-trap overhead benchmark** — measure syscall throughput vs native (e.g. a
   `read`/`futex` ping-pong microbench) to quantify when the patched-glibc fast path
   (Deferred) matters; also directly relevant to Milestone B performance.
6. **aarch64 ABI-conformance counterpart** to `libos/test/abi/x86_64/` — pin the
   entry-register contracts validated the hard way in findings #1/#5/#10/#13 (X16 resume,
   zeroed entry GPRs, restorer trampoline) as executable tests so a future refactor can't
   silently regress them.
7. **Socket edge coverage** the app tests didn't reach: IPv6 (`v6only` is in-suite but
   exercised lightly), unix-socket `SCM_RIGHTS` fd passing, `MSG_OOB`, `TCP_CORK`/
   `MSG_MORE` coalescing, non-blocking + edge-triggered epoll under many connections.

## Permanently out of scope (not just deferred)

- **`linux-sgx` and `tdx` PAL backends.** Non-portable (Intel-only ISA extensions, no
  ARM analog). ARM's confidential-computing equivalent (CCA/Realms) is architecturally
  unrelated and unsupported by this box's hardware.
- **Graphics forwarding** (`egl-redirect`, `qemu/util/sg-listener.c`). Written against
  Intel i915 ioctls; this box's GPU is NVIDIA. A separate redesign problem, independent
  of CPU architecture, and independent of Milestone B.
- **The SGX-only `iret` emulation test** (`libos/test/regression/asm/x86_64/`) — now
  skip-guarded by architecture; has no aarch64 analog at all. (The broader ABI suite is
  in "Tests worth running later" item 6.)
