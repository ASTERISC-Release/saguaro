# Example Integrations

This folder contains application integration examples. Each subdirectory has a `README.md` with
instructions to build and run the application under Gramine.

# Testing

Install the backend first (`Documentation/quickstart/`). Each example has an `exec.sh` that builds
the example and runs it under one backend. Run it in the directory of the example:

```sh
./exec.sh --vm         # also --direct, --sev (plain SEV), --sev-snp (SEV-SNP) and --sgx (Intel SGX)
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

The table lists the recorded result of each example for each backend. The last run of `exec.sh` on all backends (x86_64, aarch64, SEV, SEV-SNP, SGX, and VBS on ARM64 and x64; the RISC-V column comes from another script, see ¹¹) is in [RESULTS.md](RESULTS.md) (2026-10-20).

**Symbols:** in the VBS column a plain ✓ was measured on ARM64 and on x64 Windows 11. ✓ pass, ✗ fail, – not run or not applicable, blank not measured. 

<table>
<thead>
<tr><th rowspan="2">Example</th><th colspan="5">Sandboxed Environments</th><th colspan="5">Protected Environments</th></tr>
<tr><th>Direct x86_64</th><th>Direct aarch64</th><th>Direct riscv64</th><th>VM x86_64</th><th>VM aarch64</th><th>SEV</th><th>SEV-SNP</th><th>TDX</th><th>SGX</th><th>VBS</th></tr>
</thead>
<tbody>
<tr><td><code>bash</code></td><td>✓</td><td>✓</td><td>✓</td><td>✓</td><td>✓</td><td>✓</td><td>✓</td><td></td><td>✓</td><td>¹</td></tr>
<tr><td><code>blender</code></td><td>✓</td><td>–²</td><td>–</td><td>✓</td><td>–²</td><td>✓</td><td>✓</td><td></td><td>✓</td><td>¹</td></tr>
<tr><td><code>busybox</code></td><td>✓</td><td>✓</td><td>✓</td><td>✓</td><td>✓</td><td>✓</td><td>✓</td><td></td><td>✓</td><td>✓</td></tr>
<tr><td><code>curl</code></td><td>✓</td><td>✓</td><td>–</td><td>✓³</td><td>✓³</td><td>✓³</td><td>✓³</td><td></td><td>✓</td><td>✓</td></tr>
<tr><td><code>gcc</code></td><td>✓</td><td>✓</td><td>–</td><td>✓</td><td>✗⁶</td><td>✓</td><td>✓</td><td></td><td>✓</td><td>¹</td></tr>
<tr><td><code>graphics</code></td><td>✓⁴</td><td>✓⁴</td><td>–</td><td>✓⁴</td><td>–</td><td>–</td><td>–</td><td>–</td><td>–</td><td>–</td></tr>
<tr><td><code>helloworld</code></td><td>✓</td><td>✓</td><td>✓</td><td>✓</td><td>✓</td><td>✓</td><td>✓</td><td>✓⁵</td><td>✓</td><td>✓</td></tr>
<tr><td><code>iperf</code></td><td>✓</td><td>✓</td><td>–</td><td>✓</td><td>✓</td><td>✓</td><td>✓</td><td></td><td>✓</td><td>✓</td></tr>
<tr><td><code>lighttpd</code></td><td>✓</td><td>✓</td><td>✓</td><td>✓</td><td>✓</td><td>✓</td><td>✓</td><td></td><td>✓</td><td>✓</td></tr>
<tr><td><code>memcached</code></td><td>✓</td><td>✓</td><td>✓¹²</td><td>✓</td><td>✓</td><td>✓</td><td>✓</td><td></td><td>✓</td><td>✓</td></tr>
<tr><td><code>mongodb</code></td><td>✓</td><td>✓</td><td>–</td><td>✓</td><td>✓</td><td>✓</td><td>✓</td><td></td><td>✓</td><td>¹</td></tr>
<tr><td><code>nginx</code></td><td>✓</td><td>✓</td><td>✓</td><td>✓</td><td>✓</td><td>✓</td><td>✓</td><td></td><td>✓</td><td>✓</td></tr>
<tr><td><code>nodejs</code></td><td>✓</td><td>✓</td><td>–</td><td>✓</td><td>✓</td><td>✓</td><td>✓</td><td></td><td>✓</td><td>✗ ARM64 (x18), ✗ x64 (no x64 recipe)</td></tr>
<tr><td><code>openjdk</code></td><td>✓</td><td>✓</td><td>–</td><td>✓</td><td>✗⁶</td><td>✓</td><td>✓</td><td></td><td>✓</td><td>–</td></tr>
<tr><td><code>python</code></td><td>✓</td><td>✓</td><td>✓</td><td>✓</td><td>✗⁶</td><td>✓</td><td>✓</td><td>✓⁵</td><td>✓¹⁰</td><td>✓ ARM64, ✓⁹ x64</td></tr>
<tr><td><code>pytorch</code></td><td>✓</td><td>✓</td><td>–</td><td>✓</td><td>✗⁶</td><td>✓</td><td>✓</td><td></td><td>✓</td><td>¹</td></tr>
<tr><td><code>r</code></td><td>✓</td><td>✓</td><td>–</td><td>✓</td><td>✓</td><td>✓</td><td>✓</td><td></td><td>✓</td><td>¹</td></tr>
<tr><td><code>ra-tls/*</code></td><td>–⁷</td><td>–⁷</td><td>–</td><td>–⁷</td><td>–⁷</td><td>–⁷</td><td>–⁷</td><td></td><td></td><td>–⁷</td></tr>
<tr><td><code>redis</code></td><td>✓</td><td>✓</td><td>✓</td><td>✓</td><td>✓</td><td>✓</td><td>✓</td><td></td><td>✓</td><td>✓</td></tr>
<tr><td><code>rust</code></td><td>✓</td><td>✓</td><td>✓</td><td>✓</td><td>✓</td><td>✓</td><td>✓</td><td></td><td>✓</td><td>✓⁸</td></tr>
<tr><td><code>sqlite</code></td><td>✓</td><td>✓</td><td>✓</td><td>✓</td><td>✓</td><td>✓</td><td>✓</td><td></td><td>✓</td><td>✓</td></tr>
<tr><td><code>unit_tests</code></td><td>✓</td><td>✓</td><td>✓¹³</td><td>✓</td><td>✓</td><td>✓</td><td>✓</td><td>✓⁵</td><td>–</td><td>¹</td></tr>
<tr><td><code>vbs/*</code></td><td>–</td><td>–</td><td>–</td><td>–</td><td>–</td><td>–</td><td>–</td><td>–</td><td>–</td><td>✓</td></tr>
</tbody>
</table>

¹ No VBS recipe.  
² Not run on aarch64: the example downloads an x86_64-only binary.  
³ The server of `make check` must be on a host address that is not loopback, which the guest cannot reach: `make check HTTP_ADDR=<address>`; `exec.sh --vm`, `--sev` and `--sev-snp` pass it.  
⁴ Needs a display; run by hand.  
⁵ TDX run offline; no log in the repository.  
⁶ VM aarch64: the guest cannot map the program (`Failed to map file:/usr/bin/<program>`, `EINVAL`; `openjdk`: `Could not allocate compressed class space`).  
⁷ SGX attestation example only.  
⁸ Needs `rustup` stable with `rust-src`.  
⁹ x64 needs the research key `vbs.__insecure_skip_code_mprotect` (protection changes over packed code and data go to the data pages only). `CI-Examples/python/Makefile` sets it by default on x86_64; the HTTP test is intermittent (`dev docs: vbs-enclave/TESTING.md`).  
¹⁰ SGX: without the quote step of the check, which needs remote attestation (`RA_TYPE=dcap`, the AESM service and a PCCS).  
¹¹ Direct riscv64: native run on an Orange Pi RV2 board (Ky X1, 8 harts, 3.7 GiB) with `test/direct/test-direct.sh`, not `exec.sh`; 2026-10-20. Examples not named in that script were not run (–).  
¹² Needs `libevent-dev`; the board had none and no `sudo`, so libevent 2.1.12 was built by hand.  
¹³ The `multi-thread` and `networks` suites of `test-direct.sh`.  
