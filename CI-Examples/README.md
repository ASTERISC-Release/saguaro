# Example Integrations

This folder contains application integration examples. Each subdirectory has a `README.md` with
instructions to build and run the application under Gramine.

The table lists the recorded result of each example for each backend.

**Symbols:** ✓ pass, ✗ fail, – not run or not applicable, blank not measured. 

| Example | Direct x86_64 | VM x86_64 | Direct aarch64 | VM aarch64 | SEV | TDX | SGX | VBS |
|---|---|---|---|---|---|---|---|---|
| `bash` | ✓ | ✓ | ✓ | ✓ | ✓ |  |  | ¹ |
| `blender` | ✓ | –² | –² | –² | ✓ |  |  | ¹ |
| `busybox` | ✓ | ✓ | ✓ | ✓ | ✓ |  |  | ✓ |
| `curl` | ✓ | ✓³ | ✓ | ✓³ | ✓³ |  |  | ✓ |
| `gcc` | ✓ | ✓ | ✓ | ✗ | ✓ |  |  | ¹ |
| `graphics` | ✓⁴ | ✓⁴ | ✓⁴ |  | – |  |  | ¹ |
| `helloworld` | ✓ | ✓ | ✓ | ✓ | ✓ | ✓⁵ |  | ✓ |
| `iperf` | ✓ | ✓ | ✓ | ✓ | ✓ |  |  | ✓ |
| `lighttpd` | ✓ | ✓ | ✓ | ✓ | ✓ |  |  | ✓ |
| `memcached` | ✓ | ✓ | ✓ | ✓ | ✓ |  |  | ✓ |
| `mongodb` | ✓ | ✓ | ✓ | ✓ | ✓ |  |  | ¹ |
| `nginx` | ✓ | ✓ | ✓ | ✓ | ✓ |  |  | ✓ |
| `nodejs` | ✓ | ✓ | ✓ | ✓ | ✓ |  |  | ✗ ARM64, – x64 |
| `openjdk` | ✓ | ✓ | ✓ | ✗ | ✓ |  |  | – |
| `python` | ✓ | ✓ | ✓ | – | ✓⁶ | ✓⁵ |  | ✓ ARM64, ✗ x64 |
| `pytorch` | ✓ | ✓ | ✓ | ✗ | ✓ |  |  | ¹ |
| `r` | ✓ | ✓ | ✓ | ✓ | ✓ |  |  | ¹ |
| `ra-tls-*` | –⁷ | –⁷ | –⁷ | –⁷ | –⁷ |  |  | –⁷ |
| `redis` | ✓ | ✓ | ✓ | ✓ | ✓ |  |  | ✓ |
| `rust` | ✓ | ✓ | ✓ | ✓ | ✓ |  |  | ✓⁸ |
| `sqlite` | ✓ | ✓ | ✓ | ✓ | ✓ |  |  | ✓ |
| `unit_tests` | ✓ | ✓ | ✓ | ✓ | ✓⁶ | ✓⁵ |  | ¹ |
| `vbs/*` |  |  |  |  |  |  |  | ✓ |

¹ No VBS recipe.  
² Not run on aarch64: the example downloads an x86_64-only binary. Not run on VM x86_64: no recorded reason.  
³ `make check` serves on the host's loopback, which the guest cannot reach. Run it with the server on the container address.  
⁴ Needs a display; run by hand.  
⁵ TDX run offline; no log in the repository.  
⁶ SEV: python runs only `scripts/helloworld.py`; unit_tests runs only `thread-tls`.  
⁷ SGX attestation example only.  
⁸ Needs `rustup` stable with `rust-src`.
