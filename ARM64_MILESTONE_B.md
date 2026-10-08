# aarch64 port plan — Milestone B: the `vm`-PAL unikernel on arm64

**Status: PLANNED, not started.** This is the real end goal of the aarch64 port: getting
`gramine-vm` — the bare-metal unikernel PAL — running on arm64. Everything it needs from
the LibOS/PAL arch layers was built and validated in Milestone A; read that history, the
finding chain (#1–#13) and the registry invariants in `ARM64_MILESTONE_A.md` before
starting. The completed validation there (140/0/36 libos suite + real-world TCP servers)
is the fixed point this plan builds on.

## What this is

The unikernel PAL (`pal/src/host/vm/` frontend + `pal/src/host/vm-common/` mini-kernel,
~12k LoC) boots bare in a QEMU microvm-like guest: a ring-0 Gramine kernel (own
scheduler, drivers, page tables) running the app at ring-3 — no host OS under it. Today
it is hard-gated to x86_64 (`meson.build`: `error('VM PAL is currently available only for
x86-64 arch')`), and `vm-common` is shared with the TDX host via a compile-time
`-DHOST_TYPE=VM|TDX` switch (`pal/src/host/meson.build` includes `vm-common` when either
is enabled). The TDX side is out of scope, so the aarch64 build takes only the
`pal_vm_common_nontdx_sources` half and must not gain any new TDX coupling.

Target platform: **QEMU `virt`** (`-M virt,gic-version=3`), i.e. GICv3, PSCI, ECAM PCI,
pl011 console, DTB-driven — no exotic virtualization, all standard, KVM-on-aarch64 and
TCG both viable.

## Milestone A's concrete deliverables to this plan

Each was a hard-won finding in Milestone A:

1. The aarch64 `PAL_TCB`/`PAL_CONTEXT`/`libos_tcb` layouts + their `generated_offsets`
   discipline — the vm kernel's `pal_tcb_vm` embeds the same `PAL_TCB`
   (`common.libos_tcb`), and `syscall_asm` dispatches via `jmp *%gs:(PAL_TCB_LIBOS + 0x8)`
   whose asm comment is literally "`libos_syscall_entry` addr is at offset 0x8"
   (`kernel_events.S:138,147`) — the same TCB-slot contract Milestone A's `static_assert`
   pins.
2. **The X30-vs-X16 entry contract** (finding #1): the vm kernel's SVC trap handler will
   jump into `libos_syscall_entry` with **X16 = resume address** — no TCB field for user
   RIP is even needed then (`PAL_TCB_VM_USER_RIP` collapses into `context.regs[16]`'s
   mirror).
3. **The tagged-TPIDR + TLS-keyed registry scheme**, which applies verbatim because the vm
   kernel likewise lets the *guest* own `TPIDR_EL0` (FS-base analog, swapped per thread:
   `wrmsr(MSR_IA32_FS_BASE)` at `kernel_sched.c:184,211,241` → `msr tpidr_el0`) while
   keeping its own current-thread/per-CPU pointers in kernel-owned registers
   (GS_KERNEL_BASE → `TPIDR_EL1`).
4. **The deregistration/full-scan/ownership triangle** from findings #12–#13 — the
   registry code goes into `vm`'s build unmodified.

## Open question #0 (resolve first, ~half a day): how does the x86 deployment get LibOS into the image?

Nothing in `pal/src/host/vm*/` ever loads `libsysdb` (grep: zero `sysdb`/`libos`-loader
references), yet the README's layout lists "LibOS: close to end-of-RAM" and `syscall_asm`
indirects through the TCB's `libos_tcb+0x8` slot. The Intel/tirith tooling evidently links
or injects it outside this tree (or the fork's vm flow runs without LibOS, app-on-kernel-
API, with the jump slot written by that app's loader). Before porting the integration,
inspect the x86 tirith run/deployment scripts for how `pal.elf` + LibOS are assembled (and
the analogous flow for `tdx`, which definitely carries a LibOS).

**Default plan if unresolved:** decide it here for aarch64 — statically link `libsysdb`
(aarch64) into the single kernel image and initialize each new thread's
`libos_tcb+0x8` with `&libos_syscall_entry` at `_PalThreadCreate` time; the
`%gs:(PAL_TCB_LIBOS+0x8)` trampoline already gives the exact dispatch point, and static
linking sidesteps inventing an in-kernel ELF loader. (LibOS↔PAL calls then bind directly
at link time — the `g_pal_public_state`/`_Pal*` tables still work, just without cross-DSO
GOT indirection.)

## Phase B0 — build wiring and header split

- Lift the `meson.build` gate to allow `vm` on `x86_64` **or** `aarch64`; keep erroring on
  other arches.
- Create `pal/include/arch/aarch64/vm/{pal_host_arch.h,sigcontext.h,ucontext.h}` — the
  meson include path `../../../include/arch/@0@/vm` (`vm-common/meson.build:51`) has no
  aarch64 dir; x86's `pal_host_arch.h` is nearly empty, sigcontext/ucontext headers feed
  `pal_common_exception.c` — port to aarch64 `ucontext` shape.
- x86-only compile flags to resolve for arm: drop `-mno-red-zone` (no red zone on A64);
  the GS-based stack protector (`-mstack-protector-guard=tls -mstack-protector-guard-reg=%gs -...guard-offset=8`,
  `meson.build:193-199`) — arm64 GCC supports the analogous `-mstack-protector-guard=tpidr_el0`
  **only if the kernel guarantees TPIDR_EL0 = TCB+8 at all times, which this kernel does
  NOT** (guest owns TPIDR_EL0); simplest correct choice: global `__stack_chk_guard`
  (already the Milestone A common/ scheme) for the arm vm build.
- Source split: exclude `kernel_multicore64.S`, `kernel_acpi_madt.c`'s HOB/MADT paths
  (TDX-only per `vm-common/meson.build:38-44` and `pal_main.c:257` passing
  `hob_list_addr=NULL` anyway) from the arm build; stub
  `switch_apic_to_x2_mode()`/`apic_init()`/`xsave_init()`/`syscalls_init()` arch halves
  behind the new aarch64 files rather than #ifdefs in shared C where the x86 file *is* the
  implementation (pattern Milestone A used: `arch/aarch64/` sibling files picked by meson).

## Phase B1 — boot: drop the BIOS stage, single EL1 entry

QEMU `virt` boots `-kernel <ELF>` directly **at EL1 with MMU/D-cache off and x0 = DTB
pointer** — none of the 16/32/64-bit trampoline machinery ports; it shrinks to one
aarch64 `.S`:

- `bios/bios.S` (real-mode, EFER MSR, fw_cfg **port** 0x510 kernel-entry dance) and
  `vm_bootloader.S` (LGDT, CR3=static PML4, CR4 `PAE|OSXSAVE|...`, `xsetbv`, CR0.PG,
  `lret` to 64-bit) → one `vm_bootloader_aarch64.S`: build minimal level-3 VMSAv8 tables
  identity-mapping RAM at 0x40000000 (`TCR_EL1`: T0SZ for ≥48-bit VA, 4KB gran, ISH
  shareable; `MAIR_EL1`: attr0 normal WB-WA, attr1 device-nGnRE), `SCTLR_EL1` M|C|I, jump
  high (identity — flat map means no relocation dance), set `SP` to `_sys_initial_stack`,
  zero x0..x8 before jumping to `pal_start_c` exactly per Milestone A finding #10 (the
  *LibOS app* entry handoff gets the same zeroing later — same contract, one is boot, one
  is exec).
- Parse the DTB early (small FDT walk, ~150 LoC new): `/memory@40000000` (RAM size —
  replaces fw_cfg `FW_CFG_RAM_SIZE` and the e820 table), `/cpus` core count +
  `enable-method=psci` (replaces `FW_CFG_NB_CPUS`), `/psci` (conduit hvc|smc, `CPU_ON`
  0xC4000003), `/interrupt-router` (GICv3: `reg` of GICD + per-CPU Redists),
  `pci-host-ecam-generic` node (ECAM base + MMIO32/MMIO64 `ranges`), `/chosen`
  `stdout-path` (pl011 at 0x9000000 for early debug console — replaces COM1 0x3F8), and
  `cntfrq` from `/timer` (generic-timer tick frequency — replaces the invariant-TSC CPUID
  dance, `pal_main.c:62-90`; **under TCG this is the DT frequency, so "TSC" arithmetic
  becomes CNTVCT arithmetic with the DT-provided Hz**).
- Per-CPU pointers: `TPIDR_EL1 = &g_per_cpu_data[cpu]` (written once per CPU, replacing
  `wrmsr(MSR_IA32_GS_KERNEL_BASE)` at `kernel_multicore.c:83,296`); TPIDR_EL1 is EL1-only,
  invisible to ring-3, and never context-switched — strictly cleaner than the
  GS_BASE/GS_KERNEL_BASE pair (and removes the `swapgs` FIXMEs at `kernel_events.S:138,151,157`
  from the design entirely).
- AP bring-up: **PSCI `CPU_ON`** via the DT-conduit (`hvc`/`smc` imm from `/psci`):
  `cpu_on(target_cpu, entry, context_id)` — no INIT-SIPI-SIPI (`kernel_multicore.c:141-188`),
  no real-mode trampoline page memcpy'd to 0x8000 with planted pointers at 0x8fc0..0x8ff0
  (`kernel_multicore.S`, `kernel_multicore.h:18-21`), no MP-Wakeup mailbox (TDX-only). AP
  entry = a small A64 stub that sets TPIDR_EL1 from the context_id (= per-cpu index,
  passed in x1 — PSCI passes cookie in x1) and calls `pal_start_ap_c` — the "read BSP's
  CR3 / xsetbv / re-enable x2APIC" block becomes: nothing (shared page tables from day
  one) + `msr tpidr_el1`.

## Phase B2 — exceptions, GIC, timer

- Replace the 256-gate IDT + `isrstub` push macros + TSS/IST
  (`kernel_static_structs.S:31-65`, `kernel_events.S:20-53`) with the fixed 504-byte
  `VBAR_EL1` table (16 entries × 16 instructions; current-EL-with-SP_EL1 block is what
  matters since ring-0 runs on SP_EL1). The `isr_c` vector switch
  (`kernel_interrupts.c:80-196`) maps onto `ESR_EL1.EC`: `0x15` SVC64-from-EL0 → syscall
  path; `0x25/26` debug breakpoints (keep kgdb-ish hooks? x86 has `kernel_debug.c` via
  debugcon 0xe9 — arm: semihosting or just drop early-debug to pl011); `0x10/0x11` (EL1
  address/translation abort — Gramine's own bug → keep `triple_fault()`-equivalent:
  `brk #0` + pl011 message); EL0 data/instruction aborts (`0x24/0x21`, FAR_EL1 = the CR2
  replacement at `kernel_interrupts.c:102`) → today's #PF → memfault handling;
  "timer→`sched_thread_uninterruptable` if from userland" (vector 32) → generic-timer IRQ;
  vector 33 (invlpg IPI) **deleted outright**: `TLBI VALE1IS` is broadcast-by-architecture
  (replaces `invlpg` + IPI protocol at `kernel_memory.c:165,185` →
  `kernel_interrupts.c:146-177`; keep only a `dsb ish; isb` where ordering matters);
  vector 64 (virtio) → SPI dispatch below; vector 39 (spurious) → SError `0x17` handling.
- **GICv3 init** (new `kernel_gic.c` mirroring `kernel_apic.c`'s API surface): GICD
  (distributor: reset, route/enable SPIs to target CPU or affine-none, SGIs for IPIs),
  per-CPU RD (redistributor: enable, SRE off if using legacy banked view — simplest:
  system-register *not* required; access RD by MMIO from DT base + redistributor stride ×
  MPIDR affinity, which PSCI/DT give the mapping). vCPU index from `MPIDR_EL1` affinity
  vs `/cpus` `reg` list (replaces LAPIC ID via `kernel_apic.h:27` MSR 0x802).
- **Tick**: generic virtual timer — `CNTV_CVAL_EL0 = CNTVCT_EL0 + period*Hz`,
  `CNTV_CTL_EL0` enable, IRQ = PPI 30 (GIC INTID 26+cpu) → per-CPU EOI via
  `ICC_EOIR1_EL1` read (or MMIO path); `get_time_in_us` (`kernel_time.c:43-55`) becomes
  `(CNTVCT_EL0 - base) * 1e6 / hz`; `rdrand` → `RNDR`/`RNDRRS` if present (`-cpu max`
  exposes it), else keep whatever RNG `kernel_thread.c` uses for stack canaries.
- **Virtio interrupts**: x86 delivers ISA IRQs to vector 64 via LAPIC; on virt, PCI
  devices' INTx route through GIC SPIs — the DT `pci` node's
  `interrupt-map`/`interrupts` property gives each device's SPI at probe;
  kernel_virtio's ISR registration (`kernel_interrupts.c:309-407`'s "vector 64" fan-out)
  becomes "enable device's SPI, ISR = `virtio_isr()` fan-out by g_console/g_fs/g_vsock
  pointer" — same C fan-out shape.
- `sti`/preemption semantics: `DAIF` set/clear helpers; keep the "preempt only at timer
  IRQ when from EL0, cooperative elsewhere" design verbatim (`kernel_sched.c` is C, ~90%
  reusable; `find_next_thread`'s affinity walk unchanged).

## Phase B3 — syscall entry & context switch (the `kernel_events.S` rewrite)

`kernel_events.S` (367 lines) is the one file that must be rewritten almost entirely; the
C side (`kernel_syscalls.c` dispatch tables, `kernel_sched.c`) survives nearly untouched:

- Trap vector (SVC from EL0): save `ELR_EL1` (return site = resume addr+4) and `SP_EL0`;
  switch SP to the thread's kernel stack (or the per-CPU interrupt stack during IRQs);
  save 30 GPRs into `pal_tcb_vm.kernel_thread.context` — **use the exact `PAL_CONTEXT`
  layout Milestone A defined for aarch64** (`generated_offsets` recomputes; the 19 x86
  `PAL_TCB_VM_CONTEXT_*` GPR/RFLAGS/RIP/FPREGS offsets become the A64 reg-array offsets);
  FP state: `stp` 32×Q + `mrs fpsr/fpcr` into the per-thread area (the x86 eager-XSAVE
  design carries over: "always saved and restored", README; `kernel_xsave.c`'s XCR0/AMX
  dance → one `CPACR_EL1.FPEN=0b11` per CPU; SVE: *disable at boot* (`ZCR_EL1.LEN=0`) for
  the first milestone — NEON only, matching the `libos_xstate` scope note; lazy save later
  if measurable).
- `jump_into_user_mode`/`sysret_asm` (`kernel_events.S:116-160`): `eret` with
  `SPSR_EL1`=EL0+DAIF-unmasked, `ELR_EL1`=target, `SP_EL0`=user SP; enter at boot uses
  `TPIDR_EL0 = initial app TLS (0)` honoring the finding #10 all-GPR-zero entry contract.
- **The syscall dispatch seam**: read syscall nr (`ESR.ISS==0` → x8 per Linux A64 ABI),
  then the one instruction that connects both milestones: set **X16 = ELR_EL1−4+4 = resume
  address**, jump/call `*(tcb->common.libos_tcb + 8)` — i.e. exactly Milestone A's entry
  contract, so the *same, already-validated* `libos_syscall_entry` asm (full-table
  registry scan included: **the registry's table/`add/del/scan` trio in
  `pal_arch_exception.c` is compiled into the vm build too** — but note the vm kernel
  never *rebinds* TLS behind its own back the way the direct PAL's libc can; the scan
  stays because libsysdb's entry asm requires it, and the guest's `set_tls`-emulation path
  in `libos_clone.c` registers identically) runs unmodified inside the image.
- `save_context_and_restore_next` (`kernel_events.S:197-307`): save callee-saves + PC/SP to
  outgoing `context`, swap `TPIDR_EL0` = incoming user TLS (the FS_BASE wrmsr analog —
  Milestone A's registry keys are per-thread TLS pointers, and the vm scheduler swaps them
  exactly as the direct PAL's kernel does), `msr tpidr_el1` stays pinned (no swap —
  GS_KERNEL_BASE's per-CPU role needs no save/restore because TPIDR_EL1 is *never* visible
  to EL0), reload callee-saves, `eret` into the resumed thread (x86's path: `iretq` from
  `_isr_common` vs `sysretq` from the fast path — A64 converges both on `eret`; keep two
  entry arms only if perf wants it, not for correctness).
- **Current-thread resolution inside the kernel**: x86 `_isr_common` finds the per-CPU
  XSAVE area via GS_KERNEL_BASE then the current TCB implicitly via GS; on A64 put
  `current_thread` **in the per-CPU struct** (`per_cpu_data`: 64-byte static-asserted,
  `kernel_multicore.h:50` — grows with a `current_tcb` field) written by
  `save_context_and_restore_next`; the timer-IRQ preemption path reads
  `per_cpu_data->current_tcb` — O(1), same as GS.

## Phase B4 — memory map, page tables, PCI transport

- Constants in `kernel_memory.h:11-19` (PTs `[512MB,658MB)`, shmem `[658MB,896MB)`, PCI
  hole `[2GB,4GB)`, PAL `[1MB,4MB)`) are QEMU-*-x86-layout values; for virt (RAM base
  0x40000000): PAL/`pal.elf` loaded at 0x40000000 by QEMU (ELF `-kernel` — keep `ENTRY` a
  real load-address value, `pal.lds` ENTRY→0x40000000, ≤3MB as before); derive PT region /
  shmem ring region *relative to RAM base from DT* rather than re-hardcoding — small table
  in `kernel_memory.c:573-595`'s layout fn; keep "flat single address space, no AS,
  identity" (README) — the virtio GPA==VA assumption ("currently we have flat space",
  `kernel_virtio.c:102`) stays honored because QEMU virt exposes 1:1 RAM with no SMMU by
  default; **do not add AS switching — the identity map is a load-bearing driver
  invariant, record it as such**.
- `memory_pagetables_init` (`kernel_memory.c:402-477`, the C-built PML4 chain +
  `mov %%rax,%%cr3`): rebuild as VMSAv8 3-level-under-one-P4D walk; leaf
  `PAGE_SIZE==4096` (static_assert kept); flags: RW|U|AF|DBM|PS (attr index: normal WB for
  RAM — **AF must be set or enable HW AF via TCR.HA**; the x86 "everything RWX at boot
  then `memory_tighten_permissions()`" flow maps to PXN|UXN-based tightening; WC (`PAT`
  index 1, `kernel_memory.c:205-255`) → MAIR attr index for Device-GRE/nGRE or Normal-NC
  for the shared/virtio pages — pick MAIR slot 2 and mark shmem + ECAM + fw_cfg + GIC +
  pl011 with it at PT-build time; the AP trampoline "strong uncacheable PCD" page
  (INIT-SIPI-SIPI-era) is gone with PSCI). `invlpg`→`TLBI VALE1IS`+`DSB ISH` at the two
  call sites (`kernel_memory.c:165,185`), deleting the IPI-#33 protocol (Phase B2).
- **PCI**: keep the **virtio-pci** stack (do *not* switch to virtio-mmio transports —
  drivers stay byte-identical); config space changes from Q35 port-IO
  (`0xCF8/0xCFC`, `kernel_pci.h:25-26`) to **ECAM**: reimplement
  `vm_portio_read{b,w,l}/write{b,w,l}` (`vm/vm_callbacks.c:101-129`) as ECAM address math
  (`ecam_base + (bus<<16)|(dev<<11)|(func<<8)|reg`) with **32-bit-only access granularity
  emulated for b/w** (config space accesses are 32-bit lanes on ECAM — read-modify-shift;
  all *real* users — virtio cap walk, BARs — use the dword form, verified
  `kernel_pci.c:295-381`). Drop the Q35-only host-bridge assert (`kernel_pci.c:526-530`) —
  enumerate bus 0 for virtio device IDs directly (or accept the virt host-bridge ID); keep
  BAR relocation but take the window from the DT `ranges` MMIO32 entry (x86 hardcodes
  `[0xC0000000,0xD0000000)`; virt's MMIO32 spans 0x80000000–0x100000000 — same window works
  if RAM is ≤2GB… **no**: on virt RAM starts at 0x40000000, so cap the BAR window to a
  DT-derived non-overlapping slice and fix the allocator's search there; the ivshmem local
  mods (`kernel_pci.c:30,123-134,211-271`) belong to the input-ring/SG feature → excluded
  from arm build (out of scope, like graphics).

## Phase B5 — VMM inputs, console, and the two hardcoded rings

- fw_cfg survives on aarch64 as **MMIO** (`qemu,fw-cfg-mmio` DT node, 2 regs:
  selector/data — QEMU virt exposes it): replace the port-IO accessor pair in
  `kernel_vmm_inputs.{c,h}` (`FW_CFG_PORT_SEL 0x510`) with MMIO reads; keep every consumer
  (`opt/gramine/args|envs|pwd|unixtime_s`, `kernel_vmm_inputs.h:19-22`) verbatim — e820
  consumer is the only one replaced (DT `/memory`). Early serial: COM1 `0x3F8`
  (`kernel_debug.h:8`) → pl011 0x9000000 (both just `writeb`-style; keep the "console
  before virtio" contract).
- virtio drivers (`kernel_virtio*.c`, ~all of the I/O path): confirmed asm-free,
  port-IO-free (`vm_mmio_*`/`vm_shared_*` callbacks + plain `__atomic_*`) — port as-is
  with two touch-ups: `pause` → `yield` (`kernel_virtio_vsock.c:595`), and the fixed GPAs
  `SG_INPUT_RING_MAGIC_ADDR 0xf00200/0xf00208` (`kernel_virtio_vsock.c:98-99`) which sit
  *inside x86's PAL range* and belong to the SG input ring (`sg-listener`) deployment →
  **compile out under aarch64** with the graphics/SG feature (permanently out of scope) —
  verify the vsock code path works with the input ring disabled
  (`virtio_vsock_set_shared_input_ring(false)` is already the manifest default,
  `pal_main.c:349-355`).
- `PalSegmentBaseGet/Set` (x86: FS/GS MSRs, `pal_common_misc.c:121,137`): aarch64 has one
  user TLS reg — map `PAL_SEGMENT_TCB`→rejected, `PAL_SEGMENT_FS`→TPIDR_EL0 rd/wr via
  `mrs/msr` (kernel-mode) — the direct PAL's semantics for what libsysdb does with these
  is minimal (TLS self-load); check `libos` uses (`arch_prctl` analog absent on A64) and
  likely return `-PAL_ERROR_NOTIMPLEMENTED` for both, matching what aarch64 Linux apps
  never call.
- Randomness/cpuid-adjacent kernel code: `rdrand` (`kernel_thread.c` stack canaries etc.)
  → RNDR fallback (`CNTPCT`-seeded xorshift last resort); the kernel's `cpuid` instruction
  uses (invariant TSC leaf etc.) disappear with PSCI/DT.

## Phase B6 — LibOS integration (decision-dependent — see Open question #0)

Static-link path (default): `libsysdb` aarch64 compiles unchanged (Milestone A made it
fully arch-correct incl. `syscallas.S` full-table scan + clone/exit/exec code paths) →
link `pal.elf = vm-kernel + libsysdb + common` with `pal_symbols`/`pal.map.template`
unchanged; kernel writes `tcb->common.libos_tcb.libos_syscall_entry-slot` =
`&libos_syscall_entry` (per the static_assert pairing offsets — same assert guards both
milestones) when creating each thread; the "PAL failed Relocation of the PAL binary
failed" boot path (`setup_pal_binary`) still self-relocates the non-PIE-at-fixed-VA image
trivially (or is skipped for a `-no-pie` link). Keep the *runtime-load* alternative in
mind (an in-kernel `pal_rtld`-style loader for `libsysdb.so` near end-of-RAM à la the
README) — only if the x86 deployment inspection shows the two-binary split is
architecturally required (e.g. signed/TDX-style measured boot of LibOS). The
**LibOS-internal thread model** (each LibOS thread = kernel `struct thread` + 64KB stack,
`pal_common_threading.c`/`kernel_thread.c`) and the registry (guest TPIDR_EL0 keys) then
operate exactly as validated in Milestone A, because from LibOS's side the vm kernel is
"just another provider of the `_Pal*` API + per-thread TPIDR_EL0".

## Phase B7 — Validation (staged; each stage is a QEMU one-liner)

1. **Bare boot** (no LibOS): `-M virt,gic-version=3 -cpu cortex-a76 -m 1G -nographic
   -kernel pal.elf` + `-device virtio-serial-pci -chardev file:console.out -device
   virtconsole` + `-fw_cfg` args injection + a virtio-fs host share: pl011 early prints →
   pal_start_c init-sequence log → first thread → `pal_main` reached.
2. **Drivers**: virtio-console echo, virtio-fs
   `read_text_file_to_cstr("...manifest")`, vsock loopback (the tirith SG transport — needs
   only the vsock half, not the input ring).
3. **LibOS helloworld**: manifest + app via virtio-fs share; `libos_syscall_entry`
   round-trip via SVC; prints, exit.
4. **Milestone A suite inside the VM**: the libos regression suite can't drive the VM (no
   host exec loop under it) — validate with the tirith "run app in VM" flow: pick the
   suite's threaded cornerstones (`multi_pthread`, `exec_same`-shaped flows minus
   execve-if-unimplemented, `futex_*`, `signal_multithread`) as in-guest binaries run
   sequentially by a golden-output harness script over the virtio-fs share; the remaining
   pure-LibOS unit tests already run on the *direct* PAL and need no VM re-run (same
   code).
5. **SMP + preemption**: `-smp 4`: PSCI bring-up, 4 idle threads round-robining, timer
   preemption visible in guest.

**Risks / mitigations**: PSCI+GICv3+ECAM+fw_cfg+pl011 are all QEMU-virt standards — no
exotic bits; TCG vs KVM: generic-timer Hz differs (read from DT, never assume 1 GHz),
`wfi`/`yield` idle paths test under both; `-cpu max` SVE: masked off at boot (CPACR/ZCR)
for milestone scope; the one genuinely new kernel primitive with no Milestone A analog is
**GIC redistributor init ordering** (SGI/SPI enable, affine) — budget extra debug time
there, and the EL0↔EL1 SP-swap discipline (`SP_EL0` save/restore on every user entry/exit
— the class x86 gets from `swapgs` for free); QEMU ELF `-kernel` loading is standard but
non-PIE link at 0x40000000 must be enforced (`-nostdlib -no-pie -Wl,-Ttext`-style via
`pal.lds`, replacing the x86 "1MB + .resetvec-at-4GB" trickery entirely).

## Mechanism map (x86 `vm`-PAL today → aarch64 virt)

| x86 (`vm`-PAL today) | aarch64 (virt) equivalent |
|---|---|
| real→protected→long mode boot, GDT | single EL1 entry, `SCTLR_EL1`/`TCR_EL1`/`MAIR_EL1` |
| APIC INIT-SIPI-SIPI (LAPIC ICR MSR) multicore bring-up | PSCI `CPU_ON` (HVC/SMC from DT `/psci`) |
| ACPI MADT/HOB topology (TDX-only path) | DTB `/cpus` + `/psci` (MPIDR→vCPU-id map) |
| IDT+GDT/TSS/IST, LAPIC/IOAPIC, `iretq`/`sysretq` | `VBAR_EL1` 16-entry table, `ESR_EL1.EC` dispatch, GICv3, `eret` |
| LAPIC timer in TSC-deadline mode, invariant-TSC wall clock | generic timer `CNTV_CVAL_EL0` PPI; `CNTVCT_EL0` monotonic; fw_cfg `unixtime_s` unchanged |
| GS_BASE MSR = thread TCB, GS_KERNEL_BASE = per-CPU | `TPIDR_EL0` (guest-visible, swapped per thread) / `TPIDR_EL1` (kernel, pinned once) |
| `syscall`/`STAR`/`LSTAR`/`SFMASK` MSRs | `SVC` → sync exception → `ESR_EL1.EC==0x15` |
| PCI config space via port I/O `0xCF8/0xCFC` (Q35-assert) | ECAM MMIO window from DT (32-bit-lane emulation inside the shim) |
| virtio-pci modern-device drivers (asm-free, callback-based) | **port as-is** (only `pause`→`yield`; input-ring feature excluded) |
| x86-64 4-level paging, PWT/PCD/PAT/NX, `invlpg`+IPI#33 | VMSAv8 tables, MAIR attr indices, UXN/PXN, broadcast `TLBI VALE1IS` (IPI protocol deleted) |
| XSAVE/XRSTOR/XCR0/AMX, eager per-thread + per-CPU areas | `CPACR_EL1.FPEN`, 32×128b V regs + FPSR/FPCR to per-thread area (NEON-only; SVE masked at boot) |
| e820 via fw_cfg ports `0x510/0x514` | DTB `/memory`; fw_cfg itself stays as MMIO for args/envs/pwd/unixtime |
| `rdrand` | `RNDR` (or DT-seeded fallback) |
| `generated_offsets.c` | arch-neutral mechanism — new `PAL_TCB_VM_*`/`PER_CPU_DATA_*` offsets for the A64 `PAL_CONTEXT`; `msr-tpidr`-offset stack protector **not possible** (guest owns TPIDR_EL0) → global canary scheme |

## Milestone-A leftovers that matter during B

- The **latent registry races** listed in `ARM64_MILESTONE_A.md` ("Remaining triage
  backlog") — the vm kernel's TLS-swap scheduler touches exactly the rebind/reuse windows
  those notes describe; re-audit the invariants against the new scheduler when the
  registry starts running under the vm kernel.
- Milestone A's **"Tests worth running later"** (LTP, more real-world apps, seccomp
  overhead benchmark, aarch64 ABI-conformance suite) are not prerequisites for B but
  de-risk it: anything failing there is a shared LibOS bug best fixed before it has to be
  debugged at EL1 with only a pl011 console.

## Permanently out of scope (not just deferred)

- **`linux-sgx` and `tdx` PAL backends.** Non-portable (Intel-only ISA extensions, no ARM
  analog). ARM's confidential-computing equivalent (CCA/Realms) is architecturally
  unrelated and unsupported by this box's hardware. The aarch64 vm build must not gain new
  TDX coupling even though `vm-common` is shared with it.
- **Graphics forwarding** (`egl-redirect`, `qemu/util/sg-listener.c`) and the **SG input
  ring** (fixed GPAs, ivshmem PCI mods) — Intel i915-specific; a separate redesign
  problem, independent of CPU architecture and of this milestone.
