# Results of `./exec.sh` for the examples

The record behind the matrix in [README.md](README.md), from `./exec.sh --<backend>` in the directory of each
example. A pass means the exit status of `exec.sh` is 0 and the check of the example printed its success line.

| Backend | Platform | Result |
|---|---|---|
| `--direct`, `--vm` | Intel x86_64 | all examples pass |
| `--direct` | AArch64 | all pass |
| `--vm` | AArch64 | all pass except `gcc`, `openjdk`, `pytorch`, `python` (the guest cannot map the distribution's program: `Failed to map file ... EINVAL`) |
| `--sev`, `--sev-snp` | AMD64 (SEV, SEV-SNP) | all pass; `test/sev/test-sev.sh` passes in both modes |
| `--sgx` | Intel x86_64 | 19 of 20 pass; `python` without the attestation quote; `unit_tests-mremap` fails (the shrunk tail of a mapping stays readable); the other `unit_tests` have no SGX build |
| `--vbs` | Windows 11 ARM64 (WSL) | 11 of 12 pass; `nodejs` fails (`libstdc++` uses the reserved register x18); the 8 examples of `CI-Examples/vbs/` pass |
| `--vbs` | Windows 11 x64 (WSL) | 11 of 12 pass; `nodejs` fails at build time (no x64 recipe: the arm64 download and loader); the 8 examples of `CI-Examples/vbs/` pass |

The examples that run under `--sev` also run under TDX; `exec.sh` has no TDX backend (currently).
