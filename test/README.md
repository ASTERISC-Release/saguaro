# test/: map

Compiled from the file headers and the runners' usage text. "Not confirmed" marks a mapping taken from a
header alone or not found in any runner, workflow or document. The refactor plan is
`dev docs: test-layout/PLAN.md`.

## Commands that work today

Since phase 2 the files sit in backend folders. `test/test-direct.sh`, `test/test-vm.sh`, `test/test-sev.sh` and `test/test-sev-all.sh` are two-line forwarders that exec the new file with the same arguments; phase 4 removes them.

Run from the repository root. `test/run.sh` forwards to the runners with the arguments unchanged; `test/run.sh list` prints the backends and suites; `RUN_DRY=1` prints the command instead of running it.

| Backend | Command | Needs | Suites |
|---|---|---|---|
| direct | `test/direct/test-direct.sh [suite...]` or `test/run.sh direct [suite...]` | a direct install (`scripts/install.sh`) | `test/direct/test-direct.sh --list` |
| vm | `test/vm/test-vm.sh [suite...]` or `test/run.sh vm [suite...]` | `/dev/kvm`, a vm install (`scripts/install.sh --vm`) | libos examples hello loopback epoll outbound udp guest-loopback |
| vm, servers via the direct runner | `SAGUARO_RUNNER=saguaro-vm test/direct/test-direct.sh redis` | a vm install | server suites and `networks` |
| sev | `test/sev/test-sev.sh [suite...]` or `test/run.sh sev [suite...]` | SEV host, container of `container/run.sh` | hello memory vsock measure attest |
| sev, qualification | `test/sev/test-sev-all.sh` or `test/run.sh sev-all` | SEV-SNP host; builds with `scripts/install.sh --sev` | none (one run) |
| vbs | none | Windows machine; steps are `.cmd` files in the folders | not confirmed |
| unit tests | `test/unit/` (pytest) | none | how CI or a developer runs it: not confirmed |

## Files

Backend names: direct, vm, sev, vbs, any (not tied to a backend), graphics (vm or direct, with a
display).

### Runners and VM network tests

| File | Backend | Suite or role |
|---|---|---|
| `test/direct/test-direct.sh` | direct | the runner (pal, libos, examples) |
| `test/vm/test-vm.sh` | vm | the runner |
| `test/sev/test-sev.sh` | sev | the runner |
| `test/sev/test-sev-all.sh` | sev | qualification run |
| `test/vm/network/test-vm-loopback.sh` | vm (also sev) | vm suite `loopback`; sev suite `vsock` |
| `test/vm/network/test-vm-epoll.sh` | vm | vm suite `epoll` |
| `test/vm/network/test-vm-outbound.sh` | vm | vm suite `outbound` |
| `test/vm/network/test-vm-udp.sh` | vm | vm suite `udp` |
| `test/vm/network/test-vm-guest-loopback.sh` | vm | vm suite `guest-loopback` |
| `test/vm/network/test-vm-redis.sh` | vm | not a suite of `test-vm.sh`; called by `vm-x86_64.yml` |
| `test/vm/network/test-vm-relay.py` | vm | relay handshake and policy checks; the runner that calls it: not confirmed |
| `test/vm/vsock-loopback/` | vm | programs and servers of the VM network tests; `tests.toml` for `gramine-test` |

### Other single files

| File | Backend | Role (from header) |
|---|---|---|
| `test/boot-probe.sh` | graphics | boot time and memory commit of a graphics VM; not pass/fail |
| `test/direct/direct-prefix-sweep.sh` | direct | `fork_and_exec` regression from install copies at many path lengths |
| `test/calibrate-syscall-profile.py` | any | calibrates `scripts/syscall-profile.py`; needs strace and distro packages |
| `test/runtime-check-syscall-profile.py` | direct | compares the profile with the syscalls saguaro-direct reports as unsupported |
| `test/test-syscall-profile.py` | any | checks the profiler against raw strace output |
| `test/tolerance-syscall-profile.py` | any | runs `scripts/syscall-tolerance.py` on ten applications |

### Folders

| Folder | Backend | Content (from headers) |
|---|---|---|
| `test/unit/` | any | pytest unit tests of `graminelibos` (chroot, ldd parsing, sgx sign) |
| `test/benchmarks/` | direct | `run_benchmarks.sh` and `syscall-bench`: native against saguaro-direct |
| `test/sev/` | sev | `sev_probe`, `sev_attest`, `qmp.py`, `sev_memcheck.py`: used by `test-sev.sh` |
| `test/vm/multiproc/` | vm | multi-process checks; `check-probes.sh` is called by `vm-x86_64.yml`; `run-probes.sh`, `load.sh`, `vm-examples.sh`, `boot-loop.sh`, `sig-loop.sh`, `nginx-workers.sh`, `redis-bgsave.sh` are run by hand |
| `test/vm/multiproc-aarch64/` | vm (aarch64) | `validate.sh` and its parts (`hello.sh`, `probes.sh`, `load.sh`, LTP scripts, hang tools) |
| `test/vm/timer/` | vm, sev | timer measurements: `measure.sh` (aarch64 VM), `measure-sev.sh` (SEV-SNP), comparison scripts |
| `test/vulkan/` | graphics | Vulkan probes, `vm-check.sh`, Quake II and vkcube scripts, `Makefile` |
| `test/proton-vm/` | graphics | Proton and Wine run scripts, Windows test programs, probes |
| `test/vbs/arm64/` | vbs (ARM64) | enclave spikes: `step0-sample`, `probe`, `elf-in-image`, `cross-build` |
| `test/vbs/x64/` | vbs (x64) | the same steps for x64, plus `elf-scan.sh` and `aarch64-pp-compare.py` |

## CI

All workflows are manual (`workflow_dispatch`). Calls into `test/`: `direct-x86_64.yml` and
`direct-aarch64.yml` run `test/direct/test-direct.sh`; `vm-x86_64.yml` runs `test/vm/network/test-vm-redis.sh` and
`test/vm/multiproc/check-probes.sh`; `test/sev/test-sev-all.sh` runs the SEV checks by hand on an AMD SEV-SNP host (there is no workflow for it).
