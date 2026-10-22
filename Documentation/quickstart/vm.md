# Quickstart: Micro-Virtual Machine (`gramine-vm`)

`gramine-vm` boots the library OS, a small kernel and the application as a unikernel in a QEMU
guest (a *microVM*): the application no longer runs on the host's kernel.

## Status

Symbols as in [README.md](../../README.md):

| Configuration | Intel x86_64 | AMD64 | AArch64 | RISC-V (RV64) |
|----|----|----|----|----|
| `gramine-vm` | ✅ | ⚠️ | ⚠️ | 🛫 |
| `gramine-vm` with redirected graphics | ⚠️ | ⚠️ | 🛫 | ❌ |

## Requirements

- A **native** Linux machine of the same architecture with hardware virtualisation: `/dev/kvm`
  readable and writable by the user. On AArch64, without `/dev/kvm` the launcher falls back to QEMU's
  software emulation (TCG, `-cpu max`; `SAGUARO_ACCEL` and `SAGUARO_CPU` override): the same suites pass, about
  1.2 times slower for syscall-bound code and about 10 times slower for compute-bound code. On x86_64, KVM is required.
- For guest networking: `/dev/vhost-vsock` (module `vhost_vsock`), also accessible to the user.
  - vsock is available on most recent Linux kernels; it is enabled with `sudo modprobe vhost_vsock`
    on the host.
- `podman`. The container image already has QEMU, `virtiofsd` and the toolchain; nothing else is
  installed on the host.

# 1. Container and install

The container is opened as in [direct.md](direct.md), section 1 (`container/run.sh` for a shell,
`container/run.sh '<command>'` for one command). The vm backend is installed with
`scripts/install.sh --vm` into `~/.local/gramine-vm`:

    cd <repo-home>
    container/run.sh 'scripts/install.sh --vm'

On x86_64 `--vm` builds the direct PAL too, and the graphics paths ([graphics-vm.md](graphics-vm.md));
`--graphics disabled` leaves them out. On AArch64 it builds only the vm PAL. The vm install uses its
own build directory (`build-vm-<arch>`) and prefix, so it never overwrites a direct install.

`container/run.sh` loads `~/.local/gramine-vm/env.sh` itself when it exists, so `gramine-vm` is on
`PATH` in every shell and every one-shot command of the container. A shell that was not opened by
`container/run.sh` sources it by hand: `. ~/.local/gramine-vm/env.sh`.

# 2. Example: Hello World

Run in the container, from the directory of the example (`exec.sh` builds against the vm install,
so that the manifest names its `libsysdb.so`, and runs the program under `gramine-vm`):

    cd CI-Examples/helloworld
    ./exec.sh --vm

The output ends with:

    Hello, world
    [ VM exited with code 0 ]

The steps by hand:

    cd CI-Examples/helloworld
    make
    gramine-vm helloworld

Every other example runs the same way (`./exec.sh --vm` in its directory). The recorded results are
in [RESULTS.md](../../CI-Examples/RESULTS.md): all examples pass on Intel
x86_64; on AArch64 `gcc`, `openjdk`, `pytorch` and `python` fail in the guest (`Failed to map
file:/usr/bin/<program>`). Refer to each example folder for further per-application instructions.

# 3. Regression Tests

    test/vm/test-vm.sh libos              # the LibOS suite under gramine-vm, about 2 to 3 minutes

It ends with a pytest summary (failures with a known cause are marked xfail by
`libos/test/regression/vm_skip.toml`) and `ALL VM SUITES PASSED`. `test/vm/test-vm.sh --help`
lists the other suites.

# 4. Other Information

- Guest CPUs and size can be set using:
  - `SAGUARO_CPU_NUM`: vCPUs, default 1.
  - `SAGUARO_RAM_SIZE`: guest memory. x86_64: default `8G`; with `sys.experimental__multiprocess` (the
    default) it must be above `4G` (a 4G guest has no free GiB for the frame pool and fails to boot).
    AArch64: default `4G` with `sys.experimental__multiprocess`, `2G` without it.
    - `enclave_size` in `<manifest>.tdx` or `<manifest>.sgx`, when one of these files exists, overrides
      `SAGUARO_RAM_SIZE` (never below `1G`).
    - Memory is allocated on demand, so this is the maximum.
- By default the guest runs several processes (x86_64 and AArch64; the key `sys.experimental__multiprocess` defaults to `true`). `sys.experimental__multiprocess = false` gives the single-process guest, where `fork` fails with `Function not implemented`. The key enables `fork`, `vfork`, `execve`, pipes, signals and shared memory between processes.
- The guest has no TCP/IP stack of its own: TCP and UDP sockets are carried over vsock to a relay that the launcher starts per VM (`vm-relay: cid=... listening ...`) and that opens the real sockets on the host side. Outbound connections work by default; a server in the guest is reached through an `inbound` forward in the manifest's `[vm.net]` table or `SAGUARO_VM_INBOUND`.
- Several checkouts on one machine each get their own container and their own range of guest CIDs
  when the folder is named `gramine-<name>/gramine-multiarch` (see `Documentation/AGENTS.md`).

# 5. Troubleshooting

- `vhost-vsock: failed to open vhost device: Permission denied` (AArch64), or socket calls fail with `Address family not supported`: give the user access to `/dev/vhost-vsock` on the host (as for `/dev/kvm`) and load `vhost_vsock`.
- `exec.sh` reports that `vm` cannot run: it names the reason (not installed: run `scripts/install.sh --vm`; no `/dev/kvm`: the host has no usable KVM, or the container was created without it).
- AArch64 `Mounting "file:<app>" ... failed: ENOENT`: the binary is missing or not a PIE; rebuild it with `-fPIE -static-pie`.
