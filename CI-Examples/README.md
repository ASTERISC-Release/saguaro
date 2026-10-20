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
| `python` | ✓ | ✓ | ✓ | – | ✓⁶ | ✓⁵ |  | ✓ ARM64, ✓⁹ x64 |
| `pytorch` | ✓ | ✓ | ✓ | ✗ | ✓ |  |  | ¹ |
| `r` | ✓ | ✓ | ✓ | ✓ | ✓ |  |  | ¹ |
| `ra-tls/*` | –⁷ | –⁷ | –⁷ | –⁷ | –⁷ |  |  | –⁷ |
| `redis` | ✓ | ✓ | ✓ | ✓ | ✓ |  |  | ✓ |
| `rust` | ✓ | ✓ | ✓ | ✓ | ✓ |  |  | ✓⁸ |
| `sqlite` | ✓ | ✓ | ✓ | ✓ | ✓ |  |  | ✓ |
| `unit_tests` | ✓ | ✓ | ✓ | ✓ | ✓⁶ | ✓⁵ |  | ¹ |
| `vbs/*` |  |  |  |  |  |  |  | ✓ |

¹ No VBS recipe.  
² Not run on aarch64: the example downloads an x86_64-only binary. Not run on VM x86_64: no recorded reason.  
³ The server of `make check` must be on a host address that is not loopback, which the guest cannot reach: `make check HTTP_ADDR=<address>`; `exec.sh --vm` and `--sev` pass it.  
⁴ Needs a display; run by hand.  
⁵ TDX run offline; no log in the repository.  
⁶ SEV: python runs only `scripts/helloworld.py`; unit_tests runs only `thread-tls`.  
⁷ SGX attestation example only.  
⁸ Needs `rustup` stable with `rust-src`.  
⁹ x64 needs the research key `vbs.__insecure_skip_code_mprotect` (protection changes over packed code and data go to the data pages only). `CI-Examples/python/Makefile` sets it by default on x86_64; the HTTP test is intermittent (`Documentation/devel/vbs-enclave/TESTING.md`).
