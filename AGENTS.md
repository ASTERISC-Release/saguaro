# Running the system: instructions for AI agents

This file tells a coding agent (and a person) how to build and run this tree (Saguaro) in each of its modes. How to
**develop** here (parallel checkouts, plans, testing rules, reporting) is in
[`Documentation/AGENTS.md`](Documentation/AGENTS.md); read it before changing code. Everything is experimental.
Platform status: [`README.md`](README.md). Measured results of the examples: [`CI-Examples/RESULTS.md`](CI-Examples/RESULTS.md).

## The route

- **Use the container.** `container/run.sh` opens a shell in the development container of this machine's
  architecture, `container/run.sh '<command>'` runs one command in it, `container/run.sh --target riscv64 ...` is the
  RISC-V cross container, and `container/run.sh down` removes the container (**the install inside it is lost**).
  Nothing is built or installed on the host. The one exception is Windows VBS, which runs in WSL.
- **Install with `scripts/install.sh`** (there is no top-level `install.sh`). Options: none = the direct backend into
  `~/.local/saguaro`; `--vm` = also the vm PAL (`~/.local/saguaro-vm`); `--sev --graphics disabled` = the vm PAL with
  the SEV PAL and `gramine-sev`; `--sgx` (`~/.local/saguaro-sgx`); `--vbs` = the WSL side of the VBS tooling;
  `--target riscv64` = a cross build; `-j N` limits parallelism (use `-j4` on a small board). `scripts/install.sh --help`
  lists the rest. Load the environment with `. <prefix>/env.sh`; `container/run.sh` loads `~/.local/saguaro-vm/env.sh`
  itself.
- **Run an example with `./exec.sh --<backend>`** in its directory under `CI-Examples/`. A backend is required. It says
  why a backend cannot run, prints the command under test, streams the logs, and ends with the example's check; the
  exit status is the verdict. In a fresh container, `blender`, `mongodb`, `nodejs`, `openjdk` and `r` first stop with
  "cannot run here: missing on the host": run `CI-Examples/install-all-prereqs.sh` (or that example's
  `install-prereqs.sh`) once.

## The modes

| Mode | Needs | Install | Run | Guide |
|---|---|---|---|---|
| `direct`: an ordinary Linux process | any Linux host (container) | `scripts/install.sh` | `gramine-direct <app>`; `./exec.sh --direct` | `Documentation/quickstart/direct.md` |
| `vm`: a microVM under KVM | same-architecture host with `/dev/kvm` and `/dev/vhost-vsock` | `scripts/install.sh --vm` | `gramine-vm <app>`; `./exec.sh --vm` | `Documentation/quickstart/vm.md` |
| `sev` and `sev-snp` | AMD EPYC host with `/dev/sev`; `sev-tools/` (QEMU for SEV) beside the checkout | `scripts/install.sh --sev --graphics disabled` | `gramine-sev`; `./exec.sh --sev` or `--sev-snp`; `test/sev/test-sev.sh` | `Documentation/quickstart/sev.md` |
| `sgx` | Intel host with the in-kernel SGX driver (`/dev/sgx_enclave`) | `scripts/install.sh --sgx` | `gramine-sgx`; `./exec.sh --sgx` | `Documentation/quickstart/tdx-sgx.md` |
| `vbs`: a Windows VBS enclave | Windows 11 (ARM64 or x64) with VBS enabled, WSL | `scripts/install.sh --vbs` in WSL | `gramine-vbs <app>`; `./exec.sh --vbs` | `Documentation/quickstart/vbs-windows.md` (machine setup: `vbs-enable.md`) |
| `tdx` | Intel TDX host, a QEMU with TDX and the TD-shim checkout (as for upstream gramine-tdx) | `scripts/install.sh --vm --tdx` | `gramine-tdx <app>`; the examples that run on SEV also run on TDX (`exec.sh` has no TDX backend) | `Documentation/quickstart/tdx-sgx.md` |
| RISC-V (`direct`) | cross container, or a riscv64 board | `scripts/install.sh --target riscv64`, or plain on the board | in the QEMU guest (`scripts/riscv64/`), or `test/direct/test-direct.sh` on a board | `Documentation/quickstart/riscv.md` |
| Graphics, direct | host GPU and X server | `scripts/install.sh` | `make run-direct` in `CI-Examples/graphics/<example>` | `Documentation/quickstart/graphics-direct.md` |
| Graphics, vm (x86_64) | the QEMU fork `qemu-tirith` (outside this repository), `DISPLAY` set when the container is created | `scripts/install.sh --vm` | `make run` in `CI-Examples/graphics/<example>` with `QEMU_PATH` exported | `Documentation/quickstart/graphics-vm.md` |

Tests: `test/direct/test-direct.sh` (PAL and LibOS regression and the example suites; `SAGUARO_PREFIX` and `LOGDIR`
select the install and the logs), `test/test-vm*.sh` (the VM suites), `test/sev/test-sev.sh`, `scripts/stress_rewrite.sh`
(syscall-site rewriting), `scripts/check-docs.py` (documents). Expected results and known failures are in
`CI-Examples/RESULTS.md` (for example `test_020_cpuinfo` under SEV-SNP, `nodejs` under VBS, `unit_tests-mremap` under
SGX); a failure not listed there is new.

## Things that go wrong

- **One `gramine-vm` at a time** per machine: two instances collide on the vsock relay port ("vsock relay did not
  start: Address already in use"). Stop one before starting the other.
- **The manifest must exist** in the current directory: run `make` in the example first. A missing manifest is reported
  by `gramine-vm` as a relay failure; read the last line of the message.
- **Graphics under the VM:** export `QEMU_PATH=<qemu-tirith>/build/qemu-system-x86_64`; create the container with
  `DISPLAY` set (`DISPLAY=:0 container/run.sh`) and mount the QEMU checkout with `QEMU_TIRITH_DIR`. Run one graphics
  VM at a time. Details: `Documentation/GRAPHICS.md`.
- **SGX and rootless podman:** the enclave devices need the `exec` bind mount that `container/run.sh` already uses.
- **VBS from WSL:** run `gramine-vbs`, `make VBS=1` and `./exec.sh --vbs` in the foreground of the shell; a detached
  process has no Windows interop and fails at the Windows step.
- **Files owned by the container** (created by a rootless container) are not removable by the host user; use
  `podman unshare rm -rf <path>`.
- **A board without working DNS** fails every download with an empty file (hash `e3b0c442...`): fix the resolver first.
- **Do not report what you did not run.** A skipped or unsupported backend is "not run", not a pass. Give failing output.
