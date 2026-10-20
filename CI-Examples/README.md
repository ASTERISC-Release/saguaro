# Example Integrations

This folder contains application integration examples. Each subdirectory has a `README.md` with
instructions to build and run the application under Gramine.

# Testing

Install the backend first (`Documentation/quickstart/`). Each example has an `exec.sh` that builds
the example and runs it under one backend. Run it in the directory of the example:

```sh
./exec.sh --vm         # also --direct, --sev (plain SEV) and --sev-snp (SEV-SNP)
```

- A backend is required. Without one, `exec.sh` lists the backends the example supports and whether
  the host can run each, and exits with status 2.
- A backend that cannot run is reported with the reason (not installed, no `/dev/kvm`, no
  `/dev/sev`). A program or package missing on the host is reported with the `install-prereqs.sh`
  to run.
- The script prints each command it runs, including the command under test (lines starting with
  `+`), and shows the output as it is produced. The output is also written to `$EXEC_LOG_DIR`
  (default `/tmp/gramine-exec/`). `--keep` keeps the program output of a passing run.
- The exit status is 0 on a pass, 1 on a failure or a backend that cannot run, and 2 on a usage error.

# Results

The table lists the recorded result of each example for each backend. The last run of `exec.sh` on the
backends x86_64, aarch64, SEV and SEV-SNP is in
[`Documentation/devel/ci-matrix/results-exec.md`](../Documentation/devel/ci-matrix/results-exec.md) (2026-10-20).

**Symbols:** ✓ pass, ✗ fail, – not run or not applicable, blank not measured. 

| Example | Direct x86_64 | VM x86_64 | Direct aarch64 | VM aarch64 | SEV | SEV-SNP | TDX | SGX | VBS |
|---|---|---|---|---|---|---|---|---|---|
| `bash` | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |  |  | ¹ |
| `blender` | ✓ | ✓ | –² | –² | ✓ | ✓ |  |  | ¹ |
| `busybox` | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |  |  | ✓ |
| `curl` | ✓ | ✓³ | ✓ | ✓³ | ✓³ | ✓³ |  |  | ✓ |
| `gcc` | ✓ | ✓ | ✓ | ✗⁶ | ✓ | ✓ |  |  | ¹ |
| `graphics` | ✓⁴ | ✓⁴ | ✓⁴ | – | – | – | – | – | – |
| `helloworld` | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓⁵ |  | ✓ |
| `iperf` | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |  |  | ✓ |
| `lighttpd` | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |  |  | ✓ |
| `memcached` | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |  |  | ✓ |
| `mongodb` | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |  |  | ¹ |
| `nginx` | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |  |  | ✓ |
| `nodejs` | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |  |  | ✗ ARM64, – x64 |
| `openjdk` | ✓ | ✓ | ✓ | ✗⁶ | ✓ | ✓ |  |  | – |
| `python` | ✓ | ✓ | ✓ | ✗⁶ | ✓ | ✓ | ✓⁵ |  | ✓ ARM64, ✓⁹ x64 |
| `pytorch` | ✓ | ✓ | ✓ | ✗⁶ | ✓ | ✓ |  |  | ¹ |
| `r` | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |  |  | ¹ |
| `ra-tls/*` | –⁷ | –⁷ | –⁷ | –⁷ | –⁷ | –⁷ |  |  | –⁷ |
| `redis` | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |  |  | ✓ |
| `rust` | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |  |  | ✓⁸ |
| `sqlite` | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |  |  | ✓ |
| `unit_tests` | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓⁵ |  | ¹ |
| `vbs/*` | – | – | – | – | – | – | – | – | ✓ |

¹ No VBS recipe.  
² Not run on aarch64: the example downloads an x86_64-only binary.  
³ The server of `make check` must be on a host address that is not loopback, which the guest cannot reach: `make check HTTP_ADDR=<address>`; `exec.sh --vm`, `--sev` and `--sev-snp` pass it.  
⁴ Needs a display; run by hand.  
⁵ TDX run offline; no log in the repository.  
⁶ VM aarch64: the guest cannot map the program (`Failed to map file:/usr/bin/<program>`, `EINVAL`; `openjdk`: `Could not allocate compressed class space`).  
⁷ SGX attestation example only.  
⁸ Needs `rustup` stable with `rust-src`.  
⁹ x64 needs the research key `vbs.__insecure_skip_code_mprotect` (protection changes over packed code and data go to the data pages only). `CI-Examples/python/Makefile` sets it by default on x86_64; the HTTP test is intermittent (`Documentation/devel/vbs-enclave/TESTING.md`).
