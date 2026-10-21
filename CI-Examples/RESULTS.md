# Results of `./exec.sh` for the examples

The record behind the matrix in [README.md](README.md), from `./exec.sh --<backend>` in the directory of each
example. A pass means the exit status of `exec.sh` is 0 and the check of the example printed its success line.

In a fresh container, `blender`, `mongodb`, `nodejs`, `openjdk` and `r` first stop with "cannot run here: missing on the host"; run `CI-Examples/install-all-prereqs.sh` once (or the `install-prereqs.sh` of that example, which the message names) and repeat the command.

| Backend | Platform | Result |
|---|---|---|
| `--direct`, `--vm` | Intel x86_64 | all examples pass |
| `--direct` | AArch64 | all pass |
| `--vm` | AArch64 | all pass except `gcc`, `openjdk`, `pytorch`, `python` (the guest cannot map the distribution's program: `Failed to map file ... EINVAL`) |
| `--sev`, `--sev-snp` | AMD64 (SEV, SEV-SNP) | all pass; `test/sev/test-sev.sh` passes in both modes |
| `--sgx` | Intel x86_64 | 19 of 20 pass; `python` without the attestation quote; `unit_tests-mremap` fails (the shrunk tail of a mapping stays readable); the other `unit_tests` have no SGX build |
| `--vbs` | Windows 11 ARM64 (WSL) | 11 of 12 pass; `nodejs` fails (`libstdc++` uses the reserved register x18); the 8 examples of `CI-Examples/vbs/` pass |
| `--vbs` | Windows 11 x64 (WSL) | 11 of 12 pass; `nodejs` fails at build time (no x64 recipe: the arm64 download and loader); the 8 examples of `CI-Examples/vbs/` pass |

The examples that run under `--sev` also run under TDX; `exec.sh` has no TDX backend.

LibOS suite under `gramine-vm` (168 passed on x86_64, 167 on AArch64); under `gramine-sev` (SEV-SNP) one test
fails, `test_020_cpuinfo`: the AMD host has the `mwaitx` flag and the KVM guest does not.

## Known intermittent failure

`helloworld` under `--vbs` now and then fails its check right after its image is built and signed
(`mmap DATA_REGION: Cannot allocate memory`, printed by the example when its 512 MiB `mmap` fails): 1 of 6 rebuild-and-check
cycles, 0 of 13 checks of an image that had been run. A re-run passes.

## Changes the runs led to

- `exec.sh` needs `--sev-snp` to test SEV-SNP; `--sev` is plain SEV.
- AArch64 `vm`: the guest never received stdin (fixed in the console driver of the vm PAL).
- vm PAL, all architectures: the guest dropped vsock packets when the byte credit outlasted the ring slots, which made
  `mongodb` fail under `vm`, `sev` and `sev-snp` on a fast host (fixed by merging payloads into the newest queued packet).
- `python` under SEV and SEV-SNP: the client retries until the slowly booting guest answers.
- SGX in the container: the device nodes are bound with `exec` (a rootless `--device` mount is `noexec`, which made the
  `PROT_EXEC` mapping of the enclave fail with `EPERM`); several manifests were corrected (`helloworld`, `gcc`, `openjdk`,
  `blender`).

Not run: `graphics` (needs a display), `ra-tls/*` (SGX remote attestation).
