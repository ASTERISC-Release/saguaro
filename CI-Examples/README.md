# Example Integrations

This folder contains application integration examples. Each subdirectory has a `README.md` with
instructions to build and run the application under Gramine.

Most examples use oversimplified configurations that are not secure (for example, they mount whole
host directories into the application). Inspect and harden the configuration before using an
example as a template for your own workload.

The table lists the recorded result of each example for each backend.

| Example | Direct x86_64 | VM x86_64 | Direct aarch64 | VM aarch64 | SEV | TDX | SGX | VBS |
|---|---|---|---|---|---|---|---|---|
| `bash` | ✓ | ✓ | ✓ | ✓ | ✓ |  |  | ¹ |
| `blender` | ✓ | –² | –² | –² | ✓ |  |  | ¹ |
| `busybox` | ✓ | ✓ | ✓ | ✓ | ✓ |  |  | ✓ |
| `curl` | ✓ | ✓³ | ✓ | ✓³ | ✓³ |  |  | ✓ |
| `gcc` | ✓ | ✓ | ✓ | ✗⁴ | ✓ |  |  | ¹ |
| `graphics` | ✓⁵ | ✓⁵ | ✓⁵ |  | – |  |  | ¹ |
| `helloworld` | ✓ | ✓ | ✓ | ✓ | ✓ | ✓⁶ |  | ✓ |
| `iperf` | ✓ | ✓ | ✓ | ✓ | ✓ |  |  | ✓ |
| `lighttpd` | ✓ | ✓ | ✓ | ✓ | ✓ |  |  | ✓ |
| `memcached` | ✓ | ✓ | ✓ | ✓ | ✓ |  |  | ✓ |
| `mongodb` | ✓ | ✓ | ✓ | ✓ | ✓ |  |  | ¹ |
| `nginx` | ✓ | ✓ | ✓ | ✓ | ✓ |  |  | ✓ |
| `nodejs` | ✓ | ✓ | ✓ | ✓ | ✓ |  |  | ✗ ARM64, –⁷ x64 |
| `openjdk` | ✓ | ✓ | ✓ | ✗⁴ | ✓ |  |  | –⁸ |
| `python` | ✓ | ✓ | ✓ | –⁴ | ✓⁹ | ✓⁶ |  | ✓ ARM64, ✗ x64¹⁰ |
| `pytorch` | ✓ | ✓ | ✓ | ✗⁴ | ✓ |  |  | ¹ |
| `r` | ✓ | ✓ | ✓ | ✓ | ✓ |  |  | ¹ |
| `ra-tls-mbedtls` | –¹¹ | –¹¹ | –¹¹ | –¹¹ | –¹¹ |  |  | –¹¹ |
| `ra-tls-nginx` | –¹¹ | –¹¹ | –¹¹ | –¹¹ | –¹¹ |  |  | –¹¹ |
| `ra-tls-secret-prov` | –¹¹ | –¹¹ | –¹¹ | –¹¹ | –¹¹ |  |  | –¹¹ |
| `redis` | ✓ | ✓ | ✓ | ✓ | ✓ |  |  | ✓ |
| `rust` | ✓ | ✓ | ✓ | ✓ | ✓ |  |  | ✓¹² |
| `sqlite` | ✓ | ✓ | ✓ | ✓ | ✓ |  |  | ✓ |
| `unit_tests` | ✓ | ✓ | ✓ | ✓ | ✓⁹ | ✓⁶ |  | ¹ |
| `vbs/*` |  |  |  |  |  |  |  | ✓ |

Symbols: ✓ pass, ✗ fail, – not run or not applicable, blank not measured. VBS: ✓ means pass on
ARM64 and x64 unless a footnote says otherwise. SGX: not run on this tree's hardware (needs
`protobuf-c` and `td-shim`).

¹ No VBS recipe.  
² Not run on aarch64: the example downloads an x86_64-only binary. Not run on VM x86_64: no recorded reason.  
³ `make check` serves on the host's loopback, which the guest cannot reach. Run it with the server on the container address.  
⁴ VM aarch64: the host binary is not a PIE (gcc, pytorch, python). openjdk fails on compressed class space allocation.  
⁵ Needs a display; run by hand.  
⁶ TDX run offline; no log in the repository.  
⁷ ARM64: fails on the x18 register in the node binary and in libstdc++. x64: the recipe packs the ARM64 node.  
⁸ The JVM needs executable memory.  
⁹ SEV: python runs only `scripts/helloworld.py`; unit_tests runs only `thread-tls`.  
¹⁰ x64: NumPy and SciPy fail.  
¹¹ SGX attestation example only.  
¹² Needs `rustup` stable with `rust-src`.
