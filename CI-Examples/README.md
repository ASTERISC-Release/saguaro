# Example Integrations

This folder contains application integration examples. Each subdirectory has a `README.md` with
instructions to build and run the application under Gramine.

Most examples use oversimplified configurations that are not secure (for example, they mount whole
host directories into the application). Inspect and harden the configuration before using an
example as a template for your own workload.

The table lists the recorded result of each example for each backend.

| Example | Direct x86_64 | VM x86_64 | Direct aarch64 | VM aarch64 | SEV⁹ | TDX | SGX | VBS |
|---|---|---|---|---|---|---|---|---|
| `bash` | ✓ | ✓¹³ | ✓ | ✓ | – |  |  | –¹ |
| `blender` | ✓ | –¹¹ | –¹¹ | –¹¹ | – |  |  | –¹ |
| `busybox` | ✓ | ✓ | ✓ | ✓ | – |  |  | ✓ |
| `curl` |  |  |  |  | – |  |  | ✓ |
| `gcc` |  |  |  |  | – |  |  | –¹ |
| `graphics` | ✓¹⁷ | ✓¹⁹ | ✓¹⁸ |  | – |  |  | –¹ |
| `helloworld` | ✓ | ✓ | ✓ | ✓ | ✓ | ✓⁸ |  | ✓ |
| `iperf` |  |  |  |  | – |  |  | ✓ |
| `lighttpd` | ✓ | ✓¹² | ✓ | ✓¹² | – |  |  | ✓ |
| `memcached` | ✓ | ✓¹² | ✓ | ✓¹² | – |  |  | ✓ |
| `mongodb` |  |  |  |  | – |  |  | –² |
| `nginx` | ✓ | ✓¹² | ✓ | ✓¹² | – |  |  | ✓ |
| `nodejs` |  |  |  |  | – |  |  | ✗ ARM64, – x64⁴ |
| `openjdk` |  |  |  |  | – |  |  | –⁵ |
| `python` | ✓ | ✓¹⁴ | ✓²¹ | –²² | ✓¹⁰ | ✓⁸,¹⁰ |  | ✓ ARM64, ✗ x64⁶ |
| `pytorch` |  |  |  |  | – |  |  | –¹ |
| `r` |  |  |  |  | – |  |  | –¹ |
| `ra-tls-mbedtls` | –³ | –³ | –³ | –³ | – |  |  | –³ |
| `ra-tls-nginx` | –³ | –³ | –³ | –³ | – |  |  | –³ |
| `ra-tls-secret-prov` | –³ | –³ | –³ | –³ | – |  |  | –³ |
| `redis` | ✓ | ✓¹² | ✓ | ✓¹² | – |  |  | ✓ |
| `rust` | ✓ | ✓¹² | ✓ | ✓¹² | – |  |  | ✓⁷ |
| `sqlite` | ✓ | ✓ | ✓ | ✓ | – |  |  | ✓ |
| `unit_tests` | ✓¹⁵ | ✓¹⁶ | ✓¹⁶ | ✓¹⁶ | ✓²⁰ | ✓⁸,²⁰ |  | –¹ |
| `vbs/attestation` |  |  |  |  |  |  |  | ✓ |
| `vbs/dynhello` |  |  |  |  |  |  |  | ✓ |
| `vbs/helloworld` |  |  |  |  |  |  |  | ✓ |
| `vbs/python` |  |  |  |  |  |  |  | ✓ |
| `vbs/tcpserver` |  |  |  |  |  |  |  | ✓ |
| `vbs/tls-echo` |  |  |  |  |  |  |  | ✓ |

Symbols: ✓ pass, ✗ fail, – not run or not applicable, blank no record. A result is a record in a `TESTING.md` under `Documentation/devel/`; the date and the source of every cell are in `Documentation/devel/example-doc-refactor/FINDINGS.md`. VBS: ✓ means pass on ARM64 and x64 unless a footnote says otherwise. SGX: not run on this tree's hardware (needs `protobuf-c` and `td-shim`).

¹ No VBS recipe.  
² No VBS recipe; `mongod` uses the x18 register (known x18 blocker, ARM64).  
³ Example for SGX attestation only.  
⁴ ARM64: fails (x18 in the node binary and in libstdc++). x64: not run (the recipe packs the linux-arm64 node).  
⁵ No VBS recipe; the JVM needs executable memory.  
⁶ ARM64: passes (hello and HTTP tests; NumPy and SciPy are not packed). x64: tests 1/4 and 2/4 pass; tests 3/4 and 4/4 (NumPy, SciPy) fail.  
⁷ Needs rustup stable with `rust-src`.  
⁸ Reported by the maintainer, offline, identical to the SEV result. No TDX log is in the repository.  
⁹ Only `helloworld`, `thread-tls` (in `unit_tests/multi-thread`) and python `scripts/helloworld.py` were run under SEV. Every other example is not run.  
¹⁰ Python: only `scripts/helloworld.py` was run.  
¹¹ Blender: the example downloads an x86_64-only binary, so it cannot run on aarch64. VM x86_64: skipped, no reason recorded.  
¹² The VM runs servers through the vsock relay (`GRAMINE_VM_INBOUND`); nginx runs with `master_process off`.  
¹³ Bash: the example's 7 checks pass (7/7) only with `sys.experimental__multiprocess` in the manifest; without it 1/7. The multiprocess key is supported on x86_64 only.  
¹⁴ Python: 4/4 checks on 2026-10-12. An earlier run (2026-10-10) passed 3/4; the HTTP check was not possible then.  
¹⁵ Direct x86_64: `multi-thread` and `mremap` pass; `networks` not applicable; `align_test` is not a test (it prints a pointer and waits).  
¹⁶ `multi-thread`, `mremap` and `networks` pass; `align_test` is not a test (it prints a pointer and waits).  
¹⁷ Direct x86_64: `direct-tests`, glxgears, Quake II and SuperTuxKart ran; Minetest was not run.  
¹⁸ Direct aarch64: glxgears, Quake II, SuperTuxKart and Minetest ran on the NVIDIA GB10.  
¹⁹ VM x86_64: glxgears and Quake II ran.  
²⁰ Only `thread-tls` (in `unit_tests/multi-thread`) ran.  
²¹ Python on 2026-10-08: 4/4. On 2026-10-15 the run in another container was partial (numpy and scipy missing).  
²² Python: skipped, the host python is not PIE.  
