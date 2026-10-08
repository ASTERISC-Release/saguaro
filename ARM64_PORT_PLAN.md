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

**In progress** — clean build achieved; `helloworld` execution is not yet passing. See "Current status" below.

- Iterate `ninja -C build-aarch64/` until clean, fixing compile/link errors file by file in the order of Phases 1→4 (common → PAL → LibOS → vDSO), since later layers depend on earlier ones. **Done** — full clean build, including the test suite (`-Dtests=enabled`), confirmed via `file` showing valid aarch64 ELF shared objects for `libpal.so`/`libsysdb.so`/`vdso.so` and `ninja install` succeeding.
- Run the LibOS regression suite's smallest test first: `libos/test/regression/helloworld.c`. **Not yet green** — see below.
- Once helloworld passes, broaden to the rest of `libos/test/regression/tests.toml` incrementally — expect the signal-delivery (`prepare_sigframe`/`syscallas.S`) and clone/fork paths to surface the most bugs, since those are the parts that needed full redesigns rather than mechanical renames.

## Current status (most recent session)

**ROOT CAUSE OF THE "SLOWNESS" FOUND — it is a correctness bug after all, not a performance problem.** The previous session's conclusion ("slowness, not a loop") was wrong in an important way: there *is* an infinite loop, ~15 syscalls into `ld.so` startup, and the last session simply never observed it because GDB's own ptrace interaction masked it.

### The bug: aarch64 syscall-emulation entry clobbers the application's live link register (X30)

Reproduced deterministically with `gramine-direct helloworld` (built from this tree, `-Dlibc=glibc`, no patched guest libc). Instrumented tracing shows exactly 9 emulation round-trips (`brk → mmap → faccessat → openat → read → fstat → mmap → mmap → mprotect`), each entering *and returning correctly*; then the process spins at 100% `utime` / 0 `stime` forever. A SIGCONT-sampled backtrace at the spin point shows:

```
   ...5c4:  mov  x8, #0xd7        // munmap
   ...5c8:  svc  #0               // trapped by seccomp, emulated by Gramine
   ...5cc:  cmn  x0, #0xfff
   ...5d0:  b.cs ...5d8
=> ...5d4:  ret        with x30 = 0x...5cc   ← poisoned!
```

`ret` jumps to the `cmn`, which (return value 0, carry clear) falls through to `ret` again → a 3-instruction userspace infinite loop. So the guest never issues a 10th syscall; it just spins. The PC is always inside ld.so's `munmap` wrapper, which is precisely why the earlier SIGINT-sampling read it as "slow munmap".

**Why it happens:** the seccomp-trap emulation path installs the *resume address* into the guest context's **X30** (`maybe_emulate_syscall()` sets `context->regs[30] = pc + 4`, mirroring x86_64's `%rcx` convention). On x86_64 that is safe because the hardware `syscall` instruction itself destroys `%rcx`/`%r11` — the application can *never* hold live state there, so libc syscall wrappers reload the real return address from the stack frame. aarch64's `svc` has **no such clobber contract**: X30 at the `svc` site is the application's own live return address (e.g. ld.so's leaf-ish `munmap` wrapper's frameless `ret` target). Overwriting it and resuming via sigreturn destroys it.

Why the first 9 syscalls *appeared* to work: they are inline SYSCALL macros inside large glibc functions that had already spilled the true X30 into their stack frames and reload it in their epilogue — masking the clobber. Leaf wrappers like `munmap` (`mov x8,#nr; svc #0; cmn; b.cs; ret`) use X30 directly after the syscall and loop forever. This affects *every* leaf-style syscall wrapper in the guest libc — i.e. essentially all of them — so Milestone A can never progress past `ld.so` startup with this design.

### The fix (in progress)

Do not route the resume address through X30 at all. aarch64 has genuinely-clobberable scratch registers — X16/X17 (IP0/IP1, the linker veneer scratch) — which `syscallas.S` *already* declares sacrificed. Redesign the entry contract:
- `maybe_emulate_syscall()` / `GRAMINE_SYSCALL`: put resume-PC in **X16** (via the ucontext at `regs[16]`, which is restored on sigreturn), keep the app's live X30 saved in `context->regs[30]` untouched.
- `libos_syscall_entry`: take the resume address from X16 (AAPCS-scratch, same role RCX plays on x86_64) instead of X30; store it into `context->pc` and restore the app's true X30 from the saved frame.
- `_return_from_syscall` continues to `br` to `context->pc` (already the case), and the app's X30 comes back from `context->regs[30]` (already saved/restored) — restoring the clobbered-by-us-on-entry semantics correctly.
- `restart_syscall()` and `prepare_sigframe()` (`regs[30] = restorer`) need the same treatment: the *restorer* address legitimately goes in X30 (that mimics the `blr handler` contract — fine), but syscall-restart must not assume X30 carries anything.

### Remaining known issues (updated)

- Everything under "Known issues and their fixes" below is still fixed.
- **New, found this session:** the installed libpal.so log-spams `"A quick check."` on *every* `_PalSystemTimeQuery()` call (`pal/src/host/linux/pal_misc.c` — leftover debug `log_always` from the initial Tirith commit, x86 side too). Harmless to correctness but massive I/O overhead; removed.
- The previous session's "severe slowness of the seccomp-trap path" theory is **void** — the path never got to run more than 9 times because of the X30 clobber above. Once the fix lands, real per-syscall overhead can be measured properly; the signal-delivery round-trip cost may still be a performance topic, but the *hang* is this bug.

### (superseded) earlier status text

`helloworld` does not yet pass, but the remaining gap now looks like **severe performance, not a correctness bug**. Initial GDB sampling (plain PC snapshots via repeated SIGINT) was misleading: it kept landing on the same instruction inside the host's real `ld-linux-aarch64.so.1`'s inline `munmap` syscall wrapper, which looked like an infinite retry loop and caused a ~4-minute 100%-CPU background run to be killed as a presumed hang.

Tracing properly (breaking on `_return_from_syscall` and dumping the saved `PAL_CONTEXT` — syscall number in `x8`, args in `x0`/`x1`, resume `pc`/`sp` — on every hit) shows this was a false read: the syscall sequence is `brk → mmap → <something, nr 0x30> → openat → read → fstat → mmap → mmap → munmap`, a completely ordinary `ld.so`/libc startup sequence, each call with different arguments and a different resume address. There is no loop. It is just extremely slow: under Milestone A's design (no patched guest libc), *every single syscall* the guest's dynamic linker issues round-trips through a SIGSYS signal delivery + full context save/restore, and observed throughput was roughly one real syscall every several seconds *while GDB/Python was actively tracing it* (tracing itself adds substantial overhead on top of the already-expensive signal path).

Not yet confirmed: whether an **untraced** run (no ptrace/GDB attached) completes `ld.so`'s startup and reaches `helloworld`'s own `printf` in a tractable amount of time, or whether it's so slow that it's impractical even without tracing overhead. A plain `timeout 1200` background run (no GDB) was kicked off to settle this; result pending as of this writing. If it does complete, the remaining work is a performance question (how to make the seccomp-trap path cheaper, or revisit patching the guest libc per the deferred musl-TCB item below) rather than a bug hunt. If it does *not* complete in ~20 minutes, that itself is useful signal that something beyond raw signal-overhead is wrong, and warrants going back to the syscall-count data (how many total syscalls does a minimal `ld.so` + `helloworld` startup realistically need?) to see if the per-syscall cost or the total syscall count is the anomaly.

Debugging notes for whoever continues this:
- GDB's own pending-breakpoint-on-load mechanism does not work against Gramine's custom in-process loader (it has no `r_debug` hook GDB recognizes), so breakpoints on functions inside `libpal.so`/`libsysdb.so` have to be set by computing `runtime_base + static_offset` by hand (read `info proc mappings` for the base, `readelf -sW`/`nm` on the installed `.so` for the offset) rather than by name.
- `gdb`'s `catch syscall <name>` is a good tool for this: it fires on real kernel syscall entry/exit regardless of seccomp, so sampling `$pc`/`$x8` at each hit (and resolving `$pc` against `info proc mappings`) distinguishes PAL-internal syscalls (land in `gramine_raw_syscalls_code_begin`, the untrapped trampoline) from guest-emulated ones (land inside `libos_syscall_entry`/`_return_from_syscall`) cleanly.
- Breaking on `_return_from_syscall` specifically and reading the `PAL_CONTEXT` it's about to restore (passed in `x0`) is the most direct way to see "what syscall just got emulated, with what args, resuming where" without guessing from raw PC samples — that's what settled this one.

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
- `cd gramine-tdx/libos/test/regression && gramine-test -n tests.toml build -v && python3 -m pytest -v -k helloworld` passes, printing `Hello world!`. **Not yet passing — see "Current status" above.**
- Once that's green, run the broader `pytest -v --junit-xml libos-regression.xml` and fix failures incrementally (expected hotspots: signal delivery, clone/fork, threading — flagged above).
