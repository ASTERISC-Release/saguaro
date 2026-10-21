The `vm` PAL: the library OS and a small kernel boot as a unikernel in a QEMU/KVM guest. This
directory holds the PAL; the kernel shared with the `tdx` PAL is in `../vm-common/`. The x86_64
port uses QEMU's `q35` machine; the aarch64 port (QEMU `virt`, GICv3, PSCI, VMSAv8-64 paging) has
its sources in `arch/aarch64/` here and in `../vm-common/arch/aarch64/`. User guide:
`Documentation/quickstart/vm.md`.

# Features

- Guest memory up to 512 GB. x86_64 layout: PAL at 1 MB, page tables at [512 MB, 658 MB), memory
  shared with the host for the virtqueues at [658 MB, 896 MB), the QEMU hole and PCI at [2 GB, 4 GB),
  the rest for the LibOS and the application (`../vm-common/kernel_memory.h`).
- User / kernel separation (ring 3 / ring 0, EL0 / EL1). On x86_64, GS holds the kernel's
  per-thread TCB and KERNEL_GS the per-CPU data; FS belongs to the application.
- Processes (`sys.experimental__multiprocess`, on by default on x86_64 and aarch64): `fork`, `vfork`
  and `execve` create a process with its own address space (its own page-table root, application
  memory from a frame pool, demand paging, page protections). Process streams, handle passing,
  pipes, shared futexes and shared memory work across processes (`pal_process.c`, `pal_futex.c`,
  `pal_shm.c`). With the key set to `false` the guest runs one process and `fork` fails with
  `ENOSYS`.
- Threads: round-robin scheduling, preemptive in user mode (timer interrupt), cooperative in the
  kernel; CPU affinity; `_PalThreadResume` interrupts a blocked PAL call so that the LibOS handles a
  pending signal (`pal_threading.c`).
- Time: relative time from the TSC (x86_64) or the architected timer (aarch64), absolute time from
  the host at start. CPU 0's one-shot timer is armed for the earliest pending timeout, at most 100 ms
  ahead.
- FPU state (XSAVE on x86_64, FP/SIMD on aarch64) saved and restored on context switches and
  interrupts.
- Randomness from `rdrand` (x86_64); CPUID from the `cpuid` instruction.
- Environment variables from the host, filtered as the manifest says.
- Console (stdin, stdout) over virtio-console; files over virtio-fs (the launcher starts
  `virtiofsd` on the host's `/`).
- Networking: the guest has no TCP/IP stack. Its TCP and UDP sockets are carried over virtio-vsock
  to one relay process per VM on the host side (`python/graminelibos/vsock_relay.py`, started by the
  launcher through `tools/gramine-vm-relay.sh`), which opens the real sockets. The manifest's
  `vm.net` table sets the outbound policy and the inbound forwards. Needs `/dev/vhost-vsock`
  (`sudo modprobe vhost_vsock`).
- AddressSanitizer builds (a guest of at least 8 GB).

# Not implemented

- CPU and NUMA topology (one NUMA node).
- Full debugger support (breakpoints, backtraces and register state only).
