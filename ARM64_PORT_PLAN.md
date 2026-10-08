# aarch64 port of Gramine (gramine-tdx) — Milestone A: "linux" PAL

## End goal

The user's actual target is **`gramine-vm` (the bare-metal unikernel PAL) working on arm64** — not the `linux` PAL. This document covers **Milestone A** only: porting the LibOS/PAL arch layer using the much-smaller `linux` backend as a validation vehicle, because it exercises the ~1,550 LOC of LibOS-level arch code (syscall table, context/signal handling, TLS) that `vm`-PAL needs too, without also requiring the ~16,600 LOC of new kernel-level subsystems (GICv3, PSCI, ECAM PCI, VMSAv8-64 paging, SVE context switch, SVC-based syscalls) that the unikernel needs on top. Everything built in Phases 1-4 below (`common/arch/aarch64`, `libos/arch/aarch64` — syscall table, TCB/context struct, TLS via `TPIDR_EL0`) is shared by both PAL backends by design, specifically so it carries forward into Milestone B rather than being throwaway work. Milestone B (the actual `vm`-PAL port) is sketched at the end of this document and is the next plan to write once Milestone A is green.

## Context

Tirith's `gramine-tdx/` is a fork of Gramine that currently only builds/runs on x86_64. The user wants an incremental, step-by-step arm64 port, to be developed on this box (a native aarch64 Linux machine with KVM available but no Intel TDX/SGX and no ARM CCA/Realm support).

Two scope decisions were confirmed with the user up front:
- **No TDX/CCA.** TDX is Intel-only silicon; this box has no ARM Realm support (`rme` feature absent from `/proc/cpuinfo`). The port targets a plain, non-confidential setup.
- **No graphics forwarding yet.** The `sg-listener`/`egl-redirect` graphics path is written against Intel i915 ioctls and is a separate redesign problem (this box's GPU is NVIDIA) orthogonal to CPU architecture. Deferred entirely.

Exploration of the tree surfaced a third, bigger scope call, which this plan resolves: Gramine has **two very different PAL backends** that could be "the LibOS kernel" the user means:
- `pal/src/host/vm/` + `vm-common/` — a from-scratch bare-metal x86 unikernel (own bootloader, GDT/IDT, APIC/IOAPIC, ACPI MADT, PCI via port I/O, 4-level page tables, XSAVE, SYSCALL/SYSRET MSRs). **~16,600 LOC**, entirely x86-specific, already hard-gated to x86_64 in `meson.build:166`. Porting it means writing a new ARM unikernel from scratch (GICv3, PSCI, ECAM PCI, VMSAv8-64 tables, SVE context switching, SVC-based syscalls) — a project unto itself.
- `pal/src/host/linux/` + `linux-common/` — the ordinary backend: Gramine runs as a **regular sandboxed Linux process** (seccomp + signal handlers) on top of a real host kernel. Its x86_64-specific surface is only **~320 LOC** (a handful of small files), on top of **~1,550 LOC of LibOS-level arch code** (syscall table, context/signal-frame handling, TLS) that's unavoidable regardless of which PAL backend is used.

**Decision: target `linux` PAL first.** It's the right first milestone — it validates the entire LibOS-level arch port (syscall ABI, context switching, TLS, signal delivery) using 50x less code than the unikernel, with zero new kernel-level subsystems (no interrupt controller, no scheduler, no page tables to write). The `vm` PAL stays explicitly out of scope for this plan; it would be a much larger follow-on effort once this foundation works, and its x86→aarch64 mechanism mapping (GICv3/PSCI/ECAM/VMSAv8-64/SVE — fully enumerated during exploration) is preserved below for whoever picks that up later, but none of it is implemented here. `linux-sgx` and `tdx` PAL backends are permanently out of scope (SGX/TDX are Intel-only ISA extensions with no ARM analog).

Goal of this plan: get `gramine-tdx` building natively for aarch64 with `-Ddirect=enabled -Dvm=disabled -Dsgx=disabled -Dtdx=disabled`, and get the `helloworld` LibOS regression test (`libos/test/regression/helloworld.c`, run via `test_001_helloworld` in `libos/test/regression/test_libos.py:82-84`) passing under the `linux` PAL on this box.

## Step 0 — Branch

Create and switch to `arm64-port` off `main` before any other work. **Done.**

## Phase 1 — Shared `common/` arch layer

Smallest, most foundational layer; both PAL and LibOS depend on it. **Done.**

- Add `common/src/arch/aarch64/{cpu.c,...}` and `common/include/arch/aarch64/cpu.h` as siblings of the existing `common/src/arch/x86_64/` / `common/include/arch/x86_64/cpu.h`.
- Replace: `cpuid()`/`rdrand()`/`rdfsbase()`/`wrfsbase()`/`wrmsr()`/`rdmsr()` (no equivalents — drop or stub) → `mrs`-based ID-register reads where needed. `get_tsc()` → `mrs cntvct_el0`. `flush_tlb()`/`invlpg()` → `TLBI VAE1IS` + `DSB ISH`/`ISB` sequence. `cli()`/`sti()` → `msr daifset/daifclr`. `MB()/RMB()/WMB()` → `dmb`/`dsb` variants. `CPU_RELAX()` → `yield`.
- The x86 in/out-instruction-decode helpers (`is_x86_instr_legacy_prefix` etc., used for SGX/VM port-I/O emulation) are meaningless here — omit for the aarch64 side (they're only reachable from the out-of-scope SGX/VM/TDX backends anyway).
- `.nasm` files (`ct_memequal.nasm`, `no_exec_stack.nasm`) must become GAS `.S` — NASM doesn't target ARM at all, and `meson.build:70,77` already gates NASM usage to x86_64 only, so this is just "don't add an aarch64 NASM path."

## Phase 2 — PAL arch layer (`linux` backend only)

**Done**, with one architectural correction made after the original plan: PAL_TCB access went through several iterations (see "Known issues and their fixes" below) before landing on a tagged-`TPIDR_EL0` scheme, not the plain-global-only design originally sketched.

- `pal/src/arch/aarch64/pal_cpuid.c`: replace CPUID-leaf decoding (`pal/src/arch/x86_64/pal_cpuid.c:23`) with `MIDR_EL1`/`ID_AA64*` register reads (implementer/variant/part/revision fields instead of vendor/family/model/stepping) to fill `struct pal_cpu_info`.
- `pal/include/arch/aarch64/pal_arch.h`: the central PAL context header, parallel to `pal/include/arch/x86_64/pal_arch.h`.
  - `pal_tcb`/`pal_get_tcb()` (x86_64 version at line 32-56 reads `%gs:offset`) → aarch64 reads `TPIDR_EL0` via `mrs`, tagged with bit 0 to disambiguate "this thread's own PAL_TCB" from "the application has taken `TPIDR_EL0` over for its own libc TLS" (see below).
  - `struct PAL_CONTEXT` (x86_64: 17 GPRs + `csgsfsss`/`cr2`/`trapno` at lines 196-227) → aarch64: `x0..x30`, `sp`, `pc`, `pstate`, `fault_address` (no segment selectors, no `cr2` equivalent — use `FAR_EL1`-sourced value delivered via `sigcontext`).
  - `PAL_XREGS_STATE`/`PAL_FPSTATE` (x86 FXSAVE/XSAVE mirror, lines 124-193) → aarch64 `fpsimd_context` (32×128-bit V registers + FPSR/FPCR; no x87/MMX concept, no magic-number validation scheme).
  - `ALL_SYSCALL_ARGS` (x86_64: `rdi,rsi,rdx,r10,r8,r9`) → aarch64: `x0..x5` (syscall number in `x8`, not folded into a GPR macro the same way).
  - Drop `union pal_csgsfs` entirely (no segment registers on aarch64).
- `pal/include/arch/aarch64/linux/{pal_host_arch.h,sigcontext.h,ucontext.h,sigreturn.h,sigset.h}`: mirror Linux's real aarch64 `struct sigcontext`/`ucontext_t` layout (extensible `__reserved[]` record chain holding `fpsimd_context`/optionally `sve_context`, not a fixed FXSAVE blob). `pal_set_tcb()` drops the `arch_prctl(ARCH_SET_GS,...)` syscall entirely (doesn't exist on aarch64) and just does `msr tpidr_el0, x0` directly — no syscall needed. `ucontext_revert_syscall()` (x86: `rip -= 2`, checks for `0f 05`) → `pc -= 4`, check for `svc #0` encoding `0xd4000001`.
- `pal/src/host/linux/arch/aarch64/{pal_arch_misc.c,pal_arch_exception.c,pal_start.S}`: `_PalSegmentBaseGet/Set` → direct `mrs/msr tpidr_el0` (no `arch_prctl`). `pal_start` entry → `x0`/`x1` instead of `rdi`/`rsi`, same 16-byte stack-alignment requirement.
- `pal/src/host/linux-common/arch/aarch64/syscall.S`: rewrite `do_syscall`/`clone`/`vfork`/`syscall_rt_sigreturn`/`_PalThreadExit_asm_stub`.
  - `do_syscall`: simpler than x86 — AAPCS64 arg registers (`x0-x7`) already match the aarch64 syscall convention (`x0-x5` + nr in `x8`), no register-shuffle needed.
  - **`__NR_vfork` doesn't exist on aarch64** — must emulate via `clone(CLONE_VM|CLONE_VFORK|SIGCHLD, ...)`.
  - `syscall_rt_sigreturn`: same syscall exists, just `svc #0` instead of `syscall`.
- `stack_probe.nasm` → GAS `.S` (NASM constraint, same as Phase 1).
- `pal.lds` files: `OUTPUT_FORMAT("elf64-littleaarch64",...)` / `OUTPUT_ARCH(aarch64)` instead of the x86_64 lines; rest of the SysV section layout is unchanged.
- `pal/include/arch/aarch64/pal_topology.h`: port `pal_cache_info`/`pal_cpu_thread_info`/`pal_topo_info` (mostly arch-agnostic already) but replace the hardcoded `HUGEPAGES_2M`/`HUGEPAGES_1G` x86 sizes with aarch64's page-size-dependent huge page sizes (varies by 4K/16K/64K base page config — query at runtime rather than hardcode).

## Phase 3 — LibOS arch layer

**Done.** This was the largest and highest-risk phase; it's unavoidable regardless of PAL backend choice.

- **`libos/src/arch/aarch64/libos_table.c`** (highest priority): new syscall-number-indexed dispatch table built against aarch64's **generic syscall ABI** (`asm-generic/unistd.h`), not x86_64's legacy table. Many x86_64 table entries have no aarch64 counterpart and must be dropped in favor of syscalls Gramine already implements as the real backing logic: `open`→`openat`, `stat`/`lstat`→`newfstatat`, `access`→`faccessat`, `poll`→`ppoll`, `select`→`pselect6`, `dup2`→`dup3`, `fork`/`vfork`→`clone`, `pipe`→`pipe2`, `getdents`→`getdents64`, `mkdir`/`rmdir`/`link`/`unlink`/`symlink`/`readlink`/`chmod`/`chown`/`rename`→ their `*at` variants. Gramine's C-level handlers (e.g. `libos_syscall_open()` in `libos/src/sys/libos_open.c:129`) are already written against the modern calls, so this is largely "point the table at what already exists," not new logic.
  - New `libos/include/arch/aarch64/linux_abi/syscalls_nr_arch.h` sourced from aarch64's `<asm/unistd.h>` (numbers are completely different values from x86_64's table, no 1:1 mapping).
  - Keep Gramine's custom syscall numbers (`__NR_ACQUIRE_LLOCK`, `__NR_RELINQUISH_LLOCK`, `__NR_register_host_identity_fd`) placed consistently outside the real syscall range.
- **`libos/src/arch/aarch64/libos_context.c`**: redesign, not line-port, of `libos_context.c`.
  - FP/SIMD state: replace XSAVE/XRSTOR + CPUID-leaf feature detection (`libos_xstate_init`, `is_xstate_extended`) with aarch64's `fpsimd_context` (optionally `sve_context`, vector length queried via `PR_SVE_GET_VL`/`HWCAP_SVE`, not CPUID).
  - `restore_child_context_after_clone()`: clone-child return value goes in `x0 = 0`, not `rax`.
  - `prepare_sigframe()`: 3-arg handler convention sets `x0/x1/x2` (signo, siginfo*, ucontext*) instead of `rdi/rsi/rdx`; `pc = handler`, `sp = stack`; no `efl`/vector-count-in-`rax` analog (`pstate` instead).
  - `restart_syscall()`: `x8 = syscall_nr`, `pc = &libos_syscall_entry` (not `rax`/`rip`).
  - `maybe_emulate_syscall()`: detect `svc` 4-byte encoding instead of the 2-byte `0f 05` opcode check.
- **`libos/src/arch/aarch64/syscallas.S`**: full redesign, not a port. Gramine's LibOS intercepts syscalls in **userspace** via a patched-glibc indirect jump (see `gramine_entry_api.h` below) rather than real hardware trap semantics — x86_64's version exploits SYSCALL/SYSRET register-clobber conventions (`%rcx`/`%r11`) that have no aarch64 hardware analog. The aarch64 entry/exit trampoline should be built around a plain AAPCS64 function-call contract (`x0-x30`, `sp`, `pc`, `pstate`, `fpsimd_context`) instead.
- **`libos/src/arch/aarch64/start.S`**: straightforward port of `libos_start` — same argc/argv/envp stack-layout contract (arch-independent), just `x0/x1/w0` instead of `%rsp`-relative x86 reads, `bl`/`blr` through the GOT, 16-byte stack alignment (same AAPCS64 requirement).
- **`libos/src/arch/aarch64/libos_elf_entry.S`** (GAS, not NASM): `MXCSR`/`ldmxcsr` has no aarch64 equivalent to set up — likely simplifies to resetting `FPCR`/`FPSR` to 0 (aarch64's sane default) or doing nothing.
- **`libos/include/arch/aarch64/libos_tcb_arch.h`**: replace the XSAVE-mirror structs with `fpsimd_context`/`sve_context`; replace `LIBOS_TCB_GET`/`LIBOS_TCB_SET` (`%gs`-relative inline asm) with `TPIDR_EL0`-relative access.
- **`libos/include/arch/aarch64/libos_cpuid.h` + `libos/src/arch/aarch64/libos_cpuid.c`**: replace CPUID-leaf flag tables with `getauxval(AT_HWCAP/AT_HWCAP2)`-based ARM feature strings (`fp`, `asimd`, `aes`, `sha1`, `sha2`, `crc32`, `sve`, ...).
- **`libos/src/arch/aarch64/fs_proc/info.c`**: rewrite to match aarch64's real `/proc/cpuinfo` format (`CPU implementer`, `CPU architecture`, `CPU variant`, `CPU part`, `Features:`) instead of x86's `vendor_id`/`cpu family`/`model`/`flags`.
- **`libos/include/arch/aarch64/gramine_entry_api.h`**: the `GRAMINE_SYSCALL` macro (currently `jmpq *%gs:24`) becomes a `TPIDR_EL0`-based indirect branch; no fixed-register return-address convention needed since this isn't emulating real hardware SYSCALL semantics. (In practice: unused by any active Milestone A code path, since there is no patched guest libc — kept correct for forward-compat only.)
- **Reusable as-is** (confirmed arch-independent, just copy/symlink into the aarch64 dir): `linux_abi/types_arch.h` (standard LP64 POSIX typedefs), `linux_abi/errors_arch.h` (Linux errno table is identical on aarch64 and x86_64), `libos_internal_arch.h` (just change `EM_X86_64`→`EM_AARCH64`), `libos_vdso_arch.h` (vDSO filename string unchanged).
- **One real semantic difference to carry through, not just rename:** `RED_ZONE_SIZE` in `linux_abi/signals_arch.h` is `128` for x86_64 SysV ABI; AAPCS64 has **no red zone** — must become `0`, and anything relying on that margin needs re-checking.

## Phase 4 — vDSO arch layer

**Done.**

- `libos/src/vdso/arch/aarch64/vdso_syscall.h`: `vdso_arch_syscall()` rewritten around `svc #0` (nr in `x8`, args in `x0/x1`) for the raw-syscall path, always (no `TPIDR_EL0`-based `GRAMINE_SYSCALL` branch — vdso.so links with `--no-undefined` and never links against libpal.so, so it can't reach `g_pal_tcb_singleton` at all).
- `vdso.c`: logic is already arch-agnostic (just calls `vdso_arch_syscall(__NR_*, ...)`); only needs the new `__NR_*` constants from Phase 3's syscall-number header.
- `vdso.lds`: NOP-padding fill pattern (`text = 0x90909090`, x86 1-byte NOP) → aarch64 4-byte NOP encoding `0xd503201f` (fixed instruction width changes the fill-unit mechanics, not just the byte value).
- `vdso_note.S`: arch-agnostic, reuse unchanged.

## Phase 5 — Build system wiring

**Done.**

- Add `arch/aarch64/` sibling directories everywhere `arch/x86_64/` exists today (`pal/src/arch/`, `pal/include/arch/`, `libos/src/arch/`, `libos/include/arch/`, `libos/src/vdso/arch/`, `common/src/arch/`, `common/include/arch/`, `pal/src/host/linux/arch/`, `pal/src/host/linux-common/arch/`) — the `subdir(host_machine.cpu_family())` dispatch pattern (e.g. `pal/src/arch/meson.build:1`) needs zero changes, it already keys off this.
- `libos/meson.build:20`: the `-mfxsr -mxsave` cflags are gated `if host_machine.cpu_family() == 'x86_64'` already — leave as-is (no aarch64 branch needed, falls through cleanly). Same for the NASM/stack-protector gates in the top-level `meson.build:70,183` noted in Phase 1/2.
- Build config for this milestone: `meson setup build-aarch64/ --buildtype=release -Ddirect=enabled -Dsgx=disabled -Dvm=disabled -Dtdx=disabled -Dtests=enabled -Dlibc=glibc`. Since this box is natively aarch64, this is a **native build**, not a cross-compile — no meson cross-file needed for this phase. `-Dlibc=glibc` (not `musl`) was chosen for now: the musl *test-binary* variant needs a system `musl-gcc` package requiring sudo, unavailable in this environment; Gramine's own bundled musl runtime build (via `compile.sh`, unrelated to `musl-gcc`) is unaffected either way.

## Phase 6 — Build, fix, and validate

**Done.** Clean build; `helloworld` passes under the `linux`/`direct` PAL on aarch64 (see "Current status" below for the X30-clobber fix that unblocked it).

- Iterate `ninja -C build-aarch64/` until clean, fixing compile/link errors file by file in the order of Phases 1→4 (common → PAL → LibOS → vDSO), since later layers depend on earlier ones. **Done** — full clean build, including the test suite (`-Dtests=enabled`), confirmed via `file` showing valid aarch64 ELF shared objects for `libpal.so`/`libsysdb.so`/`vdso.so` and `ninja install` succeeding.
- Run the LibOS regression suite's smallest test first: `libos/test/regression/helloworld.c`. **Not yet green** — see below.
- Once helloworld passes, broaden to the rest of `libos/test/regression/tests.toml` incrementally — expect the signal-delivery (`prepare_sigframe`/`syscallas.S`) and clone/fork paths to surface the most bugs, since those are the parts that needed full redesigns rather than mechanical renames.

## Current status (most recent session)

**FIXED AND VALIDATED: `helloworld` PASSES.** `gramine-direct helloworld` prints `Hello world!` and exits 0 in ~10ms; `pytest -k helloworld` → `TC_01_Bootstrap::test_001_helloworld PASSED`.

**Latest full-suite scorecard: 100 passed / 41 failed / 35 skipped** (progression this "series" of sessions: 4 → 50 → 64 → 93 → 100 passed; was 4/137 at first run). The fork() cluster and the signal-delivery cluster are now functional: `pipe` passes end-to-end, `fork`/`wait`/`exec`/zombie/`double_fork`-family mostly green, and `syscall_restart` (both flavors), `itimer`, `sigaltstack`, `sigprocmask_pending`, `sighandler_reset` pass. What still fails clusters into: the deferred multi-thread PAL_TCB limitation (threaded tests), one shared internal-fault PC (`0x…11d54`-family, fault addr `0x00f00018`) across several fs/poll tests, AF_INET `getsockopt`/`socket_ioctl` ENOSYS, pseudo-FS content issues, and assorted singles — see the updated backlog below.

### What the "slowness" actually was: the seccomp-trap path clobbering the application's live X30

There *was* an infinite loop, ~9 syscalls into `ld.so` startup — a 3-instruction userspace loop inside ld.so's frameless `munmap` wrapper (`cmn → b.cs(fallthrough) → ret`, where `ret` jumped back to the `cmn`). Plain PC-sampling saw only that spin; instrumented round-trip tracing showed exactly 9 correct emulation round-trips (`brk → mmap → faccessat → openat → read → fstat → mmap → mmap → mprotect`) and then the spin at `'ret' with x30 = svc+4`.

**Root cause:** the emulation path carried the *resume address* in the guest context's **X30** (`maybe_emulate_syscall()` set `regs[30] = svc + 4`, mirroring x86_64's `%rcx`). x86_64 can do this safely because the hardware `syscall` instruction itself destroys `%rcx` — no application can hold live state there at a syscall site, so libc wrappers reload the real return address from their stack frame. aarch64's `svc` destroys **nothing**: X30 at an `svc` site is the application's own **live link register**. Installing the resume address over it (and sigreturning that) corrupts every leaf-style libc syscall wrapper — which `ret`s via X30 immediately after the `svc` — into a self-loop. The first 9 syscalls survived only because their glibc call sites are large functions that reload the true X30 from their stack frame, masking the clobber until the first frameless wrapper (`munmap`).

**Fix — new entry contract, resume address in X16** (X16/X17, AAPCS64 IP0/IP1, are the architecturally-sanctioned clobber set and were *already* declared sacrificed by `syscallas.S`):
- `maybe_emulate_syscall()` (aarch64 `libos_context.c`): resume → `context->regs[16]`; `regs[30]` untouched (plain app state, saved/restored by the ucontext converters as always).
- `libos_syscall_entry` (aarch64 `syscallas.S`): entry contract now "X16 = resume address"; the TCB GOT chain is rebuilt around X17, the arriving X16 is stored into `context->pc` *and* mirrored into `context->regs[16]` (the restart invariant), and the app's X30 is saved as ordinary state.
- `_return_from_syscall`: restores X16 from `regs[16]` and branches to `context->pc` via X17; the app's X18 reloads after the SP switch via the still-live context base, and X19 via a self-load `ldr x19, [x19, #off]`.
- `restart_syscall()` needed no logic change — `regs[16]`'s mirror already carries the correct resume through `_return_from_syscall`'s X16 restore (invariant documented in both places).
- `GRAMINE_SYSCALL` macro (both the `.S` and C-inline-asm copies in `gramine_entry_api.h`): now `adr x16, 1f; <GOT chain via x17>; br x17` instead of `blr x16` — delivers the resume per the new contract and never touches X30, exactly matching real `svc` clobber semantics. (Previously it was the *only* correct path, which is why the `GRAMINE_SYSCALL`-based tests passed while every real-libc test spun.)

**Also fixed:** removed a leftover debug `log_always("A quick check.")` in `_PalSystemTimeQuery()` (`pal/src/host/linux/pal_misc.c`, an initial-Tirith-commit leftover also affecting x86) that fired on *every* clock query — pure I/O overhead.

**Performance takeaway:** the earlier "one syscall every several seconds" reading was entirely a GDB/ptrace artifact; untraced, the full `ld.so` + `helloworld` startup through the SIGSYS round-trip path takes ~10ms wall. The seccomp-trap-per-syscall design may still cost throughput on syscall-heavy workloads (the patched-libc fast path remains the eventual answer, per the Deferred section), but there is no anomaly left to hunt.

### Debugging notes (still useful for future aarch64 signal-path work)

- GDB's pending-breakpoint-on-load does not work against Gramine's custom in-process loader (no `r_debug` hook), so breakpoints in `libpal.so`/`libsysdb.so` need hand-computed `runtime_base + static_offset`.
- `catch syscall <name>` fires on real kernel syscall entry/exit regardless of seccomp; sampling `$pc`/`$x8` distinguishes PAL-internal syscalls (in `gramine_raw_syscalls_code_begin`) from guest-emulated ones cleanly.
- **Attaching GDB to a spinning/seccomp-trapped process can mask the spin** (ptrace signal interplay let the guest sprint past the loop while traced) — instrumented `log_always` tracing at `libos_emulate_syscall`/`handle_sync_signal` plus a `kill -CONT`-triggered SIGCONT catchpoint snapshot was what actually localized this bug.
- Running the harness outside this repo's container needs `PATH=/home/user/.local/gramine/bin` *and* `PYTHONPATH=/home/user/.local/gramine/lib/python3.12/site-packages` (the `gramine-test`/`gramine-manifest` tools don't find each other otherwise), and `gramine-test -n tests.toml build` must run before `pytest` (manifests are generated artifacts).

### Second blocker found & fixed: the aarch64 glibc runtime was missing its loader (packaging, not port code)

First full-suite run after the X30 fix: **50 passed / 91 failed / 35 skipped**. Triage showed the bulk of the failures (nearly all of `TC_01_Bootstrap` beyond the passing subset, most of `TC_30_Syscall`/`TC_80_Socket`) were one harness-level cause, not 90 bugs: `error: libos_init() failed in init_elf_objects: ENOENT` — `find_interp()` could not resolve every test binary's `PT_INTERP` (`/lib/ld-linux-aarch64.so.1`) inside the manifest's `/lib` chroot. Cause: `glibc_output_paths` in the glibc subproject meson.build (source of truth: `subprojects/packagefiles/glibc-2.39/meson.build`) listed the loader only for x86_64 (`elf/ld-linux-x86-64.so.2`), so the installed aarch64 runtime dir had libc/libm/libpthread/etc. but **no `ld-*` at all**. Only the ~28 manifests whose `LD_LIBRARY_PATH` also mounts the host `/lib/aarch64-linux-gnu` escaped this — which is why `helloworld` and the early `TC_01` tests passed while almost nothing else did. Fixed by adding the matching aarch64 branch: `elf/ld-linux-aarch64.so.1`, `elf/ld.so`, plus `mathvec/libmvec.so{,.1}` (libmvec is built for aarch64 since glibc 2.28; same optional-dir pattern as the x86_64 entry). `compile.sh` builds the glibc tree generically and needed no changes — the loader binary was already in the build tree, just never installed.

Spot-checks after the fix (all previously ENOENT): `console`, `toml_parsing`, `argv_from_manifest`, `debug_log_inline` now pass. `uid_and_gid` fails rc=2 (to investigate; plausibly a host-uid-dependent assertion). `multi_pthread` now gets *past* loader startup and dies with `Stack protector: Gramine internal stack corruption detected` / internal memory fault — i.e. it lands exactly on the **deferred multi-threaded PAL_TCB limitation** below, confirming that deferral analysis with a concrete failing repro instead of theory. Full-suite re-run pending.

### Third blocker fixed: aarch64 `clone()` raw-ABI argument slots differ from x86_64's — every guest `fork()` became EFAULT

Symptom (after the loader fix): every `fork()`-using test fails identically — `pipe`, `mkfifo`, `shm`, `sid`, `fdleak`, `open_opath`, `fcntl_lock_child_only`, `eventfd_fork`, … — with `fork(): Bad address` (EFAULT), while `execve`-only tests pass.

Diagnosis chain (worth recording — each tier contributed a distinct fact):
1. `strace` of the guest shows `clone(flags=CLONE_CHILD_CLEARTID|CLONE_CHILD_SETTID|SIGCHLD, child_tidptr=0x...)` — strace decodes aarch64 correctly, so *something* non-NULL is being passed as ctid.
2. Gramine's own debug log for the same call: `Getting TLS from current thread` (CLONE_SETTLS unset — correct) followed by EFAULT, which can only come from `libos_syscall_clone()`'s `is_user_memory_writable(child_tidptr)` check reading it as **0**.
3. Dumping the raw sigcontext `x0-x5` inside the PAL's SIGSYS handler for `__NR_clone`: `x0=0x1200011 x1=0 x2=0 x3=0 x4=<TPIDR_EL0-relative tcb field>`.
4. glibc source + disassembly of the *installed* runtime settle the ABI: `_Fork` hand-places `flags→x0`, zeros in `x1..x3`, **ctid→x4**; aarch64's `clone.S` carries the svc-register contract as its own comment: `X0:flags, x1:newsp, x2:parenttidptr, x3:newtls, x4:childtid` (the C-level `clone()` takes 7 args — `fn,stack,flags,arg,ptid,tls,ctid` — and shuffles them).

**Root cause:** `libos_syscall_clone(flags, user_stack_addr, parent_tidptr, child_tidptr, tls)` is written to x86_64's slot order and the aarch64 syscall table entry (`libos/src/arch/aarch64/libos_table.c:257`) calls it with raw positional args. On aarch64 the guest libc puts **TLS in x3 and child_tidptr in x4** — swapped relative to Gramine's parameter list. So on every `fork()`: Gramine reads `child_tidptr` = x3 = 0 → EFAULT (the check is strict); and on every `pthread_create` (CLONE_SETTLS set) it reads `tls` = x4 = the *ctid* pointer instead of x3's real TLS base — silently wrong TLS for every guest thread, which makes it a prime suspect inside several multi-thread crash cases too (re-verify after the fix; distinct from the deferred multi-threaded PAL_TCB limitation, though possibly entangled with it).

**Fix:** as planned, implemented in commit `abba8520`: `libos_syscall_clone_raw()` registered at `[__NR_clone]` in the aarch64 table, tail-calling the unchanged `libos_syscall_clone()` with the x3/x4 slots exchanged, plus the NULL-`child_tidptr` tolerance under `CLONE_CHILD_SETTID` re-added as kernel-alike hardening with corrected rationale. The EFAULT vanished from every fork-using test immediately.

### Fourth blocker fixed: the forked child crashed in the `pal_get_tcb()` singleton fallback right after `set_tls()` (infinite silent SIGSEGV)

Symptom after the slot fix: fork no longer failed, but every forking test *hung*, with the child in a hot infinite `SIGSEGV`/`rt_sigreturn` loop (fault address `0x10`) inside `rs_thread()` at the load following the `msr tpidr_el0` — the parent waited forever for the child's checkpoint-creation ack. (A prior hypothesis — checkpoint blob clobbering libsysdb GOT slots — was an adrp-arithmetic artifact: the GOT slots used are `fault_pc_page + 0xa4000`, i.e. the `0x7ffffff7 8xxx` rw segment, not `load_base + 0xbc000`; verified slot contents were intact. Debugging note: on aarch64, `adrp` resolves relative to the *PC page*, and `libsysdb`'s `.text` is mapped ~0xa1KB above its load base, so GOT addresses must be computed from the faulting PC's page, not the ELF base.)

**Root cause:** `rs_thread()` (the checkpoint-restore thread relocator in `bookkeep/libos_thread.c`) runs `set_tls(tcb->context.tls)` — the first point in a *forked child* where `TPIDR_EL0` stops holding the tagged PAL_TCB. The next inlined `pal_get_tcb()` therefore takes the untagged `g_pal_tcb_singleton` fallback — but the fork-child boot path never reaches `libos_init()`'s `pal_tcb_singleton_snapshot_before_user_entry()` call (which only runs *after* `receive_checkpoint_and_restore()` returns), and the singleton slot itself had been checkpoint-restored to the *parent's* PAL_TCB address, whose first word read as zero in the child (the parent's PAL_TCB lived in an anonymous region the checkpoint blob restores as fresh zero pages — pointers to it survive, contents don't). Fallback dereferenced NULL; the memfault handler's silent-internal path returned to the same faulting instruction forever.

**Fix (commit `67acb91d`):** snapshot the singleton from the child's own still-tagged `TPIDR_EL0` (the register survives the blob restore; the memory doesn't) immediately before `rs_thread()`'s `set_tls()`. `pipe` was the first test to pass fork end-to-end on aarch64.

### Fifth blocker fixed: signal handlers had no restorer on aarch64 — every handler `ret`urned to address 0

Symptom: handlers ran (prints appeared) but the process then died silently (or with "illegal instruction" logs) right after the handler's `ret` — the entire "signal-frame crash" backlog (`itimer`, `sigaltstack`, `sigprocmask_pending`, `sighandler_sigpipe`, `syscall_restart` test 2, …).

**Root cause:** Gramine's `prepare_sigframe()` sets the handler's return address from the caller-provided `sa_restorer` — the x86_64 mechanism (glibc's x86 `sigaction()` wrapper injects `SA_RESTORER` + a libc-.text restorer stub). aarch64's glibc passes *no* restorer (`SET_SA_RESTORER` in its `libc_sigaction.c` only forwards a user-provided one; observed `sa_flags = SA_RESTART` with no `SA_RESTORER` bit): on arm64 Linux the **kernel** writes `mov x8, #139 (__NR_rt_sigreturn); svc #0` into the signal frame and points LR at it — verified empirically on this box (native handler's x30 = frame-stack address, words `0xd2801168`/`0xd4000001`). The port inherited only the SA_RESTORER half, so LR = NULL on every delivery.

**Fix:** aarch64 `prepare_sigframe()` ignores the restorer argument (the arm64 kernel ignores `SA_RESTORER` too) and points LR at a two-instruction `mov x8,#139; svc #0` trampoline in libsysdb's own executable .text — deliberately *not* placed in the frame (Gramine guest stacks are non-executable; making them executable per signal would be a real security regression for zero benefit, since only SP matters to `rt_sigreturn`, never the trampoline's own address). Its `svc` executes from Gramine-internal text, which `illegal_upcall()`'s "internal execution" check would abort on (that check is what makes x86 guest restorers invisible: they're in guest-visible libc text) — so the check now whitelists exactly that one site: PC within the 16-byte trampoline window *and* the trapped word is the `svc #0` encoding (`0xd4000001`; PC matching is offset-independent because the PAL reverts PC to the trapping instruction before upcalling — see known-issue #6).

### Sixth blocker fixed: restarted syscalls ran with their first argument clobbered by the `-ERESTART*` return code

Discovered while landing the trampoline (test 1 of `syscall_restart` still failed `read: EBADF` even after signal delivery worked): on aarch64 **x0 is both arg0 and the return-value register**. `do_syscall()` unconditionally writes the return value into x0 — including the `-ERESTARTSYS`/`-ERESTARTNOHAND`/`-ERESTARTNOINTR` codes — *before* `restart_syscall()` re-dispatches the syscall, so the restart saw arg0 = the code: `wait4(-1)` restarted as `wait4(-512)` (PGID 512 → spurious `ECHILD` — the `mkfifo`/`sid`/`open_opath`/`fdleak` "wait error: No child processes" failures even with fork working) and `read(fd)` restarted with `fd = -ERESTARTSYS` → EBADF. x86_64 is immune: the return slot `%rax` is distinct from arg0's `%rdi`.

**Fix:** mirror the arm64 kernel's own remedy (`pt_regs->orig_x0`, which exists for exactly this reason): new `PAL_CONTEXT.orig_x0` snapshotted in `do_syscall()` right before dispatch, restored by aarch64's `restart_syscall()`. The unconditional return-value write itself *must* stay — `handle_signal()`'s `-ERESTART*` classification in `libos_signal.c` reads it back via `pal_context_get_retval()` to decide restart-vs-`-EINTR` (a first attempt that deferred the write broke that classification and was reworked into this shape). The field never travels through guest signal frames; it is re-saved per dispatch. `syscall_restart` now passes both flavors.

**Remaining triage backlog (current 41 failures, clustered):**
- **Deferred multi-thread PAL_TCB limitation (~15):** `multi_pthread`, `fp_multithread`, `signal_multithread`, `signalexit_multithread`, `sigterm_multithread`, `futex_bitset`/`futex_requeue`/`futex_wake_op`, `sigaction_per_process` (hangs), `pthread_set_get_affinity`, spinlock/rwlock unittests, OpenMP `simple_for_loop`. Note the TLS-slot fix (commit `abba8520`) removed the *wrong-TLS* corruption these threads used to get, but the tagged-`TPIDR_EL0` singleton still can't serve >1 guest thread — the musl-`struct pthread`-anchored scheme in "Deferred" is the real fix. `exit_group` (hangs after forking a child, worth a probe: may be an ack-race, not threading).
- **One shared internal-fault PC, fault addr `0x00f00018` (~8):** `shm`, `close_range`, `devfs`, `device_passthrough`, `keys`, `poll`, `procstat`, `console` — all die identically (`Internal memory fault at 0x00f00018 (0x7ffffff2…)`), one bug, not eight. Faulting PC sits in libsysdb ~`+0x65dc4`-ish region; worth symbolizing the exact instruction once.
- **AF_INET socket ops ENOSYS (~3):** `getsockopt`, `socket_ioctl`, `socket_udp` (`udp: server socket: Function not implemented`) — missing handlers, mechanical.
- **Pseudo-FS content issues (~4):** `proc`, `cpuinfo`, `sysfs`, `synthetic` — Phase 3's aarch64 `/proc/cpuinfo` rewrite implicated; `synthetic` prints dirents and seems to stop early.
- **`stat_invalid_args`:** the test itself prints "stat/lstat do not exist on this architecture; skipping" but the pytest wrapper still asserts the x86 expected string — a *test-suite* expectation gap for aarch64 (rc=0 from the binary), not a Gramine bug. Low priority / cosmetic.
- **`syscall_redirect` (rc=2), `gettimeofday` (emits binary garbage to stdout?), `exec_same`, `eventfd`/`eventfd_read_then_write` (fork+eventfd races — may share a root cause with `fdleak`-style ack timing), `basic_bootstrapping_static` (SIGSEGV with a static-pie binary), `mmap`-adjacent singles:** probe individually.
- `asm/x86_64/iret_emulation`: fails by construction, SGX/x86-only, no aarch64 analog (already listed as out of scope).
- `libos/test/regression/root/` is an untracked test-build artifact (chroot target for `tmp_enc` mounts) — leave untracked.
- Debugging note for the GOT/singleton work above: on aarch64, `adrp` targets are PC-page-relative, and libsysdb maps its `.text` ~0xa1KB above its load base — GOT-slot runtime addresses must be computed from the *faulting PC's page* (+0xa4000 here), not `load_base + 0xbc000`; getting this wrong produced a misleading "GOT slots contain strings/zeroes" red herring for a while.

### Known issues and their fixes, this session (all resolved except the munmap hang above)

Fixed, in order of discovery, all now committed:

1. **PAL_TCB unreachable in `libpal.so`'s own bootstrap.** `libpal.so` is a self-executing, no-`PT_INTERP` ET_DYN binary (no real dynamic linker ever processes its relocations) — but `g_pal_tcb_singleton` had default (exported) visibility, so the compiler emitted GOT-indirect loads for it even from within `libpal.so` itself, and that GOT entry is never populated. Fixed by giving the symbol `protected` visibility (still exported for LibOS's cross-object GOT access, but allows direct, relocation-free addressing from within `libpal.so`) and hand-writing `pal_get_tcb()`/`pal_set_tcb()` with explicit direct vs. GOT-indirect addressing depending on `IN_PAL`.
2. **`pal_rtld.c`'s `perform_relocations()` only recognized x86_64 relocation type constants** (`R_X86_64_RELATIVE`/`GLOB_DAT`/`JUMP_SLOT`). Added `R_AARCH64_RELATIVE`/`GLOB_DAT`/`JUMP_SLOT`/`ABS64` to `pal/include/elf/elf.h` and an arch-conditional `RELOC_TYPE_*` indirection.
3. **Stack-protector canary read during `libpal.so`'s own self-relocation.** aarch64 falls back to GCC's generic `__stack_chk_guard`-based stack protector (no register-relative scheme the way x86_64 has); that symbol has the same GOT-before-relocation problem as (1), and it's read in literally every protected function's prologue — including `perform_relocations()` itself. Fixed by disabling stack-protector instrumentation for the whole of `pal_rtld.c` (the only code that ever runs before self-relocation completes) via `#pragma GCC optimize("no-stack-protector")`.
4. **`topo_info.c`'s cache topology code treated a missing `physical_line_partition` sysfs file as fatal**, but this box's kernel doesn't expose it for some cache indices even though sibling attributes are present. Made it default to 0 on `-ENOENT`, matching the tolerance already given to other optional sysfs attributes in the same file.
5. **`clone()`'s stack-adjustment broke AArch64's mandatory 16-byte SP alignment.** `syscall.S`'s `clone` reserved `0x18` (24) bytes on the child's stack for `f`/`arg`/`exit_func` — not a multiple of 16 — so the new thread's initial SP was misaligned, and its first `ldr x9, [sp]` took an SP-alignment fault (SIGBUS). Fixed by reserving `0x20` (32) instead.
6. **`illegal_upcall()` misattributed an ordinary guest syscall as "Gramine internal" fatal corruption.** For a SIGSYS/seccomp trap, `handle_sync_signal()` was passing `info->si_addr` as the "faulting instruction address" — but the kernel does not populate `si_addr` meaningfully for SIGSYS (it supplies `si_call_addr` instead, already consumed by `ucontext_revert_syscall()`). The wrong address made `lookup_vma()` fail to find any VMA, triggering `illegal_upcall`'s "else fatal" branch for completely ordinary guest syscalls. This bug is architecture-generic (present in shared `pal_exception.c`) but only ever exercised on an architecture (aarch64, with no patched guest libc) that routes *all* ordinary syscalls through this seccomp-trap path rather than hitting it in rare corner cases. Fixed by using the already-reverted `rip` for the SIGSYS case instead of `si_addr`.
7. **The single-threaded PAL_TCB stopgap broke the instant Gramine's own IPC worker thread started**, not just when the guest itself used threads. `g_pal_tcb_singleton` is one process-wide global; Gramine always spawns an internal IPC worker thread during LibOS init (independent of whether the guest app is multi-threaded), and that thread's own `pal_set_tcb()` call overwrote the global, corrupting the main thread's view of `get_cur_thread()` for the rest of execution. Root-caused with the user, who chose a narrow fix over patching musl's `struct pthread` (patching musl would not actually have fixed this specific bug: Gramine's internal threads are created via a raw `clone()` with no musl `struct pthread` at all, so there'd be nothing to patch a field into for them). Fixed with a tagged-`TPIDR_EL0` scheme: every thread's own (architecturally independent, per-thread) `TPIDR_EL0` holds its PAL_TCB pointer with bit 0 set as a tag (real pointers are ≥8-byte aligned, so that bit is otherwise always clear); `pal_get_tcb()` checks the tag first and only falls back to `g_pal_tcb_singleton` when `TPIDR_EL0` is untagged (i.e. has been taken over by the running application's own libc TLS). The one thread that ever needs the fallback (whichever thread runs application code) gets `g_pal_tcb_singleton` snapshotted via a new `pal_tcb_singleton_snapshot_before_user_entry()`, called right before `TPIDR_EL0` is handed to application TLS in both `libos_init.c` (process start) and `libos_exec.c` (`execve()`). This still carries forward the originally-accepted "single-threaded *application*" limitation unchanged — it just no longer breaks on Gramine's own, unrelated internal threads.

### Deferred (explicitly, not forgotten)

- **Real per-thread PAL_TCB for a multi-threaded guest application.** Out of scope for `helloworld` (single-threaded). If a multi-threaded guest test is attempted later, the tagged-`TPIDR_EL0` fallback (`g_pal_tcb_singleton`) will only ever be correct for whichever guest thread last ran `execute_elf_object()`/`execve()` — a second or later guest-created thread will read the wrong PAL_TCB once its own `TPIDR_EL0` is taken over by its own libc TLS (via `clone_implementation_wrapper()`'s `set_tls()` in `libos_clone.c`). The real fix discussed with the user — patching musl's `struct pthread` to carry a Gramine-owned field, populated at thread-creation time, reached via `TPIDR_EL0` + a fixed offset — is still believed correct for *that* case and is the next thing to implement if/when guest threading is tested. (It would need to run alongside the tagged-`TPIDR_EL0` scheme above, not replace it, since Gramine's own internal threads still have no musl `struct pthread` to anchor to.)

## Milestone B (the real end-goal, next plan after this one): `vm`-PAL unikernel port

Not implemented in this plan — tracked here so the Milestone A work is deliberately shaped to feed into it. Once `helloworld` passes under `linux`-PAL on aarch64, re-scope a Milestone B plan covering the bare-metal unikernel (`pal/src/host/vm/` + `vm-common/`, currently hard-gated x86_64-only at `meson.build:166`). x86→aarch64 mechanism map gathered during exploration, for that future plan:

| x86 (`vm`-PAL today) | aarch64 equivalent |
|---|---|
| real→protected→long mode boot, GDT | single EL1 entry, `SCTLR_EL1`/`TCR_EL1`/`MAIR_EL1` |
| APIC INIT-SIPI-SIPI (LAPIC ICR MSR) multicore bring-up | PSCI `CPU_ON` (HVC) |
| ACPI MADT topology (TDX/HOB-only; plain-KVM path uses fw_cfg already, reusable) | devicetree `/cpus` (only needed if fw_cfg path insufficient) |
| IDT+GDT/TSS/IST, LAPIC/IOAPIC, `iretq` | `VBAR_EL1` vector table + `ESR_EL1.EC` dispatch, GICv3, `eret` |
| LAPIC timer, TSC-deadline MSR | GICv3 PPI + generic timer (`CNTV_*_EL0`) |
| GS-base MSR as TCB pointer, XSAVE context switch | `TPIDR_EL1`/`TPIDR_EL0` (same registers Milestone A establishes), NEON/SVE load-store gated by `CPACR_EL1` |
| `syscall`/`STAR`/`LSTAR`/`SFMASK` MSRs | `SVC` → sync exception → `ESR_EL1.EC==SVC64` |
| PCI config space via port I/O `0xCF8`/`0xCFC` (Q35-specific) | ECAM MMIO window (aarch64 has no port I/O at all) |
| virtio device drivers (MMIO capability model) | **already portable as-is, no changes needed** |
| x86-64 4-level paging, PWT/PCD/PAT/NX bits, `invlpg` | VMSAv8-64 tables, `MAIR_EL1` attr indices, `UXN`/`PXN`, `TLBI VAE1IS`+barriers |
| XSAVE/XRSTOR, XCR0, CPUID leaf 0xD | NEON/SVE load-store, `CPACR_EL1`, runtime-queried SVE vector length |
| `generated_offsets.c` mechanism | architecture-neutral already — just needs aarch64 struct content once the above lands |

Key reused asset from Milestone A: the `TPIDR_EL0`/`TPIDR_EL1`-based TCB access pattern established in Phases 2-3 is the same primitive the unikernel's per-CPU/per-thread context pointer needs — this is the concrete link between the two milestones, not just shared intent.

## Permanently out of scope (not just deferred)

- **`linux-sgx` and `tdx` PAL backends.** Non-portable (Intel-only ISA extensions, no ARM analog). ARM's confidential-computing equivalent (CCA/Realms) is architecturally unrelated and unsupported by this box's hardware.
- **Graphics forwarding** (`egl-redirect`, `qemu/util/sg-listener.c`). Written against Intel i915 ioctls; this box's GPU is NVIDIA. A separate redesign problem, independent of CPU architecture, and independent of Milestone B.
- **ABI conformance test suite** (`libos/test/abi/x86_64/`, NASM-based) and the SGX-only `iret` emulation test (`libos/test/regression/asm/x86_64/`) — worth an aarch64 counterpart eventually for the entry-register contract, but not required for the helloworld milestone, and the `iret` test specifically is SGX-only so has no aarch64 analog at all.

## Verification

- `ninja -C build-aarch64/` completes with no errors. **Passing.**
- `cd gramine-tdx/pal/regression && gramine-test build -v && python3 -m pytest -v --junit-xml pal-regression.xml` — PAL-level regression. **Not yet run.**
- `cd gramine-tdx/libos/test/regression && gramine-test -n tests.toml build -v && python3 -m pytest -v -k helloworld` passes, printing `Hello world!`. **Passing** (`TC_01_Bootstrap::test_001_helloworld PASSED`).
- Once that's green, run the broader `pytest -v --junit-xml libos-regression.xml` and fix failures incrementally (expected hotspots: signal delivery, clone/fork, threading — flagged above).
