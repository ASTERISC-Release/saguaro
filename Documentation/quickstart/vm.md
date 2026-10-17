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
  readable and writable by your user. Emulation (TCG) is not supported, but it *might* work.
- For guest networking: `/dev/vhost-vsock` (module `vhost_vsock`), also accessible to your user.
  - vsock is available on most recent Linux kernels; you can enable it with `sudo modprobe vhost_vsock`.
- `podman` for the recommended container build route.

# 1. Build

The container image already has QEMU, `virtiofsd` and the toolchain:

    cd <repo-home>
    ./container/run.sh                   # shell in the container; the rest runs inside
    ./scripts/install.sh --vm            # build and install into ~/.local/gramine-vm
    . ~/.local/gramine-vm/env.sh

On x86_64 `--vm` builds the direct PAL too, and the graphics paths ([graphics-vm.md](graphics-vm.md)); `--graphics disabled` leaves them out. On AArch64 it builds only the vm
PAL.

**Host route** (Debian/Ubuntu, needs sudo for the packages; commands from `INSTALL.md`):

    sudo apt install qemu-system-x86 virtiofsd lsof python3-psutil       # x86_64
    sudo apt install qemu-system-arm virtiofsd device-tree-compiler      # AArch64
    ./scripts/dependencies.sh --build-only
    ./scripts/install.sh --vm
    . ~/.local/gramine-vm/env.sh

On x86_64, if `command -v virtiofsd` finds nothing, link `/usr/libexec/virtiofsd` into
`/usr/local/bin`.

# 2. Example: Hello World

    cd CI-Examples/helloworld
    make                               # against the vm install: the manifest names its libsysdb.so
    gramine-vm helloworld

The output ends with:

    Hello, world
    [ VM exited with code 0 ]

Refer to each example folder for further per-application build/run instructions.

# 3. Regression Tests

    test/test-vm.sh libos              # the LibOS suite under gramine-vm, about 2 to 3 minutes

It ends with a pytest summary (failures with a known cause are marked xfail by
`libos/test/regression/vm_skip.toml`) and `ALL VM SUITES PASSED`. `test/test-vm.sh --help`
lists the other suites.

# 4. Other Information

- Guest CPUs and size can be set using:
  - `GRAMINE_CPU_NUM`: vCPUs, default 1.
  - `GRAMINE_RAM_SIZE`: guest memory. x86_64: default `8G`, at least `4G`
    - Note that memory is allocated on-demand, so this is the maximum.
- By default the guest runs one process: `fork` fails with `Function not implemented`. The experimental key `sys.experimental__multiprocess = true` enables `fork`, `vfork`, `execve`, pipes, signals and shared memory between processes.
- The guest has no TCP/IP stack of its own: TCP and UDP sockets are carried over vsock to a relay that the launcher starts per VM (`vm-relay: cid=... listening ...`) and that opens the real sockets on the host side. Outbound connections work by default; a server in the guest is reached through an `inbound` forward in the manifest's `[vm.net]` table or `GRAMINE_VM_INBOUND`.

# 5. Troubleshooting

- `vhost-vsock: failed to open vhost device: Permission denied` (AArch64), or socket calls fail with `Address family not supported`: give your user access to `/dev/vhost-vsock` (as for `/dev/kvm`) and load `vhost_vsock`.
- AArch64 `Mounting "file:<app>" ... failed: ENOENT`: the binary is missing or not a PIE; rebuild it with `-fPIE -static-pie`.
