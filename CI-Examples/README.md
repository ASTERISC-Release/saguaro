# Example Integrations

This folder contains application integration examples. For instructions how to build and 
run each application under Gramine, please see the README.md in each subdirectory.

Note that most of the examples use oversimplified configurations which
are not secure (e.g., they mount whole host directories into the application).
If you take these examples as templates for your own workloads, please inspect
and harden the configurations.

| Example | Direct x86_64 | VM x86_64 | Direct aarch64 | VM aarch64 | SEV | TDX | SGX | VBS |
|---|---|---|---|---|---|---|---|---|
| `bash` |  |  |  |  |  |  |  | –¹ |
| `blender` |  |  |  |  |  |  |  | –¹ |
| `busybox` |  |  |  |  |  |  |  | ✓ |
| `curl` |  |  |  |  |  |  |  | ✓ |
| `gcc` |  |  |  |  |  |  |  | –¹ |
| `graphics` |  |  |  |  |  |  |  | –¹ |
| `helloworld` |  |  |  |  |  |  |  | ✓ |
| `iperf` |  |  |  |  |  |  |  | ✓ |
| `lighttpd` |  |  |  |  |  |  |  | ✓ |
| `memcached` |  |  |  |  |  |  |  | ✓ |
| `mongodb` |  |  |  |  |  |  |  | –² |
| `nginx` |  |  |  |  |  |  |  | ✓ |
| `nodejs` |  |  |  |  |  |  |  | ✗ ARM64, – x64⁴ |
| `openjdk` |  |  |  |  |  |  |  | –⁵ |
| `python` |  |  |  |  |  |  |  | ✓ ARM64, ✗ x64⁶ |
| `pytorch` |  |  |  |  |  |  |  | –¹ |
| `r` |  |  |  |  |  |  |  | –¹ |
| `ra-tls-mbedtls` |  |  |  |  |  |  |  | –³ |
| `ra-tls-nginx` |  |  |  |  |  |  |  | –³ |
| `ra-tls-secret-prov` |  |  |  |  |  |  |  | –³ |
| `redis` |  |  |  |  |  |  |  | ✓ |
| `rust` |  |  |  |  |  |  |  | ✓⁷ |
| `sqlite` |  |  |  |  |  |  |  | ✓ |
| `unit_tests` |  |  |  |  |  |  |  | –¹ |
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
