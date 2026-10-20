# Changelog

What this tree added to upstream [gramine-tdx](https://github.com/gramineproject/gramine-tdx), newest first.
Status words as in [README.md](README.md). Open items are in [TODO.md](TODO.md).

## 2026-10-20

- `./exec.sh --direct | --vm | --sev | --sev-snp | --sgx | --vbs` in every `CI-Examples/` directory, with a check
  behind each; results in [CI-Examples/RESULTS.md](CI-Examples/RESULTS.md).
- Intel SGX verified: `scripts/install.sh --sgx`, `container/run.sh` passes the SGX devices.
- vm PAL: AArch64 guests get stdin; vsock receive no longer drops packets under load.
- `make VBS=1` no longer builds the runtime; `scripts/install.sh --vbs` installs it.
- Quickstart guides use the container only.

## 2026-10-17 to 2026-10-16

- Timer granularity in the vm PAL (x86_64, AArch64, SEV-SNP): the one-shot timer fires at the earliest timeout.
- AMD SEV and SEV-SNP for the vm PAL: `gramine-sev`, attestation reports checked against AMD's certificates.

## 2026-10-15 to 2026-10-14

- Windows VBS enclaves (`vbs` PAL), ARM64 and x64: threads, signals, sockets, Python, servers and attestation.
- AArch64: `AT_HWCAP`/`AT_HWCAP2`, multi-process in the vm PAL, accelerated graphics under `gramine-direct`.
- More LibOS system calls (xattrs, vectored I/O, memfd, SysV IPC, inotify, timers); multi-process in the x86_64 vm PAL.
- Accelerated graphics for `gramine-direct` on x86_64 (host GPU), Vulkan, Proton feasibility.
- System-call matrices from LTP (x86_64 and AArch64).

## 2026-10-12 to 2026-10-09

- TCP and UDP in the vm PAL through a vsock relay.
- RISC-V direct PAL (experimental, emulated guest), syscall interception without a patched libc, host libc option.
- Application syscall profiler and checker.
- AArch64 direct PAL (complete) and vm PAL (experimental); graphics host redirection.
