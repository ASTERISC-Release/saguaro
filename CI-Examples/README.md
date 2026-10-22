# Example Integrations

This folder contains application integration examples. Each subdirectory has a `README.md` with
instructions to build and run the application under Gramine.

# Testing

Install the backend first (`Documentation/quickstart/`). Each example has an `exec.sh` that builds
the example and runs it under one backend. Run it in the directory of the example:

```sh
./exec.sh --vm         # also --direct, --sev (plain SEV), --sev-snp (SEV-SNP), --sgx (Intel SGX) and --vbs (Windows VBS)
```

- A backend is required. Without one, `exec.sh` lists the backends the example supports and whether
  the host can run each, and exits with status 2.
- A backend that cannot run is reported with the reason (not installed, no `/dev/kvm`, no
  `/dev/sev`). A program or package missing on the host is reported with the `install-prereqs.sh`
  to run.
- The script prints each command it runs, including the command under test (lines starting with
  `+`), and shows the output as it is produced. The output is also written to `$EXEC_LOG_DIR`
  (default `/tmp/saguaro-exec/`). `--keep` keeps the program output of a passing run.
- The exit status is 0 on a pass, 1 on a failure or a backend that cannot run, and 2 on a usage error.

# Results

The table lists the result of each example for each backend. The record of the `exec.sh` runs (x86_64, aarch64, SEV, SEV-SNP, SGX, and VBS on x86_64 and aarch64) is in [RESULTS.md](RESULTS.md); the RISC-V column comes from another script (see ¹⁰). The examples that run on SEV also run on TDX; `exec.sh` has no TDX backend.

**Symbols:** ✓ pass, ✗ fail, – not run or not applicable, blank not measured.

<table>
<thead>
<tr><th rowspan="2">Example</th><th colspan="5">Sandboxed Environments</th><th colspan="6">Protected Environments</th></tr>
<tr><th>Direct x86_64</th><th>Direct aarch64</th><th>Direct riscv64</th><th>VM x86_64</th><th>VM aarch64</th><th>SEV</th><th>SEV-SNP</th><th>TDX</th><th>SGX</th><th>VBS x86_64</th><th>VBS aarch64</th></tr>
</thead>
<tbody>
<tr><td><code>bash</code></td><td>✓</td><td>✓</td><td>✓</td><td>✓</td><td>✓</td><td>✓</td><td>✓</td><td>✓</td><td>✓</td><td>¹</td><td>¹</td></tr>
<tr><td><code>blender</code></td><td>✓</td><td>–²</td><td>–</td><td>✓</td><td>–²</td><td>✓</td><td>✓</td><td>✓</td><td>✓</td><td>¹</td><td>¹</td></tr>
<tr><td><code>busybox</code></td><td>✓</td><td>✓</td><td>✓</td><td>✓</td><td>✓</td><td>✓</td><td>✓</td><td>✓</td><td>✓</td><td>✓</td><td>✓</td></tr>
<tr><td><code>curl</code></td><td>✓</td><td>✓</td><td>–</td><td>✓³</td><td>✓³</td><td>✓³</td><td>✓³</td><td>✓³</td><td>✓</td><td>✓</td><td>✓</td></tr>
<tr><td><code>gcc</code></td><td>✓</td><td>✓</td><td>–</td><td>✓</td><td>✗⁵</td><td>✓</td><td>✓</td><td>✓</td><td>✓</td><td>¹</td><td>¹</td></tr>
<tr><td><code>graphics</code></td><td>✓⁴</td><td>✓⁴</td><td>–</td><td>✓⁴</td><td>–</td><td>–</td><td>–</td><td>–</td><td>–</td><td>–</td><td>–</td></tr>
<tr><td><code>helloworld</code></td><td>✓</td><td>✓</td><td>✓</td><td>✓</td><td>✓</td><td>✓</td><td>✓</td><td>✓</td><td>✓</td><td>✓</td><td>✓</td></tr>
<tr><td><code>iperf</code></td><td>✓</td><td>✓</td><td>–</td><td>✓</td><td>✓</td><td>✓</td><td>✓</td><td>✓</td><td>✓</td><td>✓</td><td>✓</td></tr>
<tr><td><code>lighttpd</code></td><td>✓</td><td>✓</td><td>✓</td><td>✓</td><td>✓</td><td>✓</td><td>✓</td><td>✓</td><td>✓</td><td>✓</td><td>✓</td></tr>
<tr><td><code>memcached</code></td><td>✓</td><td>✓</td><td>✓¹¹</td><td>✓</td><td>✓</td><td>✓</td><td>✓</td><td>✓</td><td>✓</td><td>✓</td><td>✓</td></tr>
<tr><td><code>mongodb</code></td><td>✓</td><td>✓</td><td>–</td><td>✓</td><td>✓</td><td>✓</td><td>✓</td><td>✓</td><td>✓</td><td>¹</td><td>¹</td></tr>
<tr><td><code>nginx</code></td><td>✓</td><td>✓</td><td>✓</td><td>✓</td><td>✓</td><td>✓</td><td>✓</td><td>✓</td><td>✓</td><td>✓</td><td>✓</td></tr>
<tr><td><code>nodejs</code></td><td>✓</td><td>✓</td><td>–</td><td>✓</td><td>✓</td><td>✓</td><td>✓</td><td>✓</td><td>✓</td><td>✗ (no x64 recipe)</td><td>✗ (x18)</td></tr>
<tr><td><code>openjdk</code></td><td>✓</td><td>✓</td><td>–</td><td>✓</td><td>✗⁵</td><td>✓</td><td>✓</td><td>✓</td><td>✓</td><td>–</td><td>–</td></tr>
<tr><td><code>python</code></td><td>✓</td><td>✓</td><td>✓</td><td>✓</td><td>✗⁵</td><td>✓</td><td>✓</td><td>✓</td><td>✓⁹</td><td>✓⁸</td><td>✓</td></tr>
<tr><td><code>pytorch</code></td><td>✓</td><td>✓</td><td>–</td><td>✓</td><td>✗⁵</td><td>✓</td><td>✓</td><td>✓</td><td>✓</td><td>¹</td><td>¹</td></tr>
<tr><td><code>r</code></td><td>✓</td><td>✓</td><td>–</td><td>✓</td><td>✓</td><td>✓</td><td>✓</td><td>✓</td><td>✓</td><td>¹</td><td>¹</td></tr>
<tr><td><code>ra-tls/*</code></td><td>–⁶</td><td>–⁶</td><td>–</td><td>–⁶</td><td>–⁶</td><td>–⁶</td><td>–⁶</td><td></td><td></td><td>–⁶</td><td>–⁶</td></tr>
<tr><td><code>redis</code></td><td>✓</td><td>✓</td><td>✓</td><td>✓</td><td>✓</td><td>✓</td><td>✓</td><td>✓</td><td>✓</td><td>✓</td><td>✓</td></tr>
<tr><td><code>rust</code></td><td>✓</td><td>✓</td><td>✓</td><td>✓</td><td>✓</td><td>✓</td><td>✓</td><td>✓</td><td>✓</td><td>✓⁷</td><td>✓⁷</td></tr>
<tr><td><code>sqlite</code></td><td>✓</td><td>✓</td><td>✓</td><td>✓</td><td>✓</td><td>✓</td><td>✓</td><td>✓</td><td>✓</td><td>✓</td><td>✓</td></tr>
<tr><td><code>unit_tests</code></td><td>✓</td><td>✓</td><td>✓¹²</td><td>✓</td><td>✓</td><td>✓</td><td>✓</td><td>✓</td><td>–</td><td>¹</td><td>¹</td></tr>
<tr><td><code>vbs/*</code></td><td>–</td><td>–</td><td>–</td><td>–</td><td>–</td><td>–</td><td>–</td><td>–</td><td>–</td><td>✓</td><td>✓</td></tr>
</tbody>
</table>

¹ No VBS recipe.  
² Not run on aarch64: the example downloads an x86_64-only binary.  
³ The server of `make check` must be on a host address that is not loopback, which the guest cannot reach: `make check HTTP_ADDR=<address>`; `exec.sh --vm`, `--sev` and `--sev-snp` pass it.  
⁴ Needs a display; run by hand.  
⁵ VM aarch64: the guest cannot map the program (`Failed to map file:/usr/bin/<program>`, `EINVAL`; `openjdk`: `Could not allocate compressed class space`).  
⁶ SGX attestation example only.  
⁷ Needs `rustup` stable with `rust-src`.  
⁸ x64 needs the research key `vbs.__insecure_skip_code_mprotect` (protection changes over packed code and data go to the data pages only). `CI-Examples/python/Makefile` sets it by default on x86_64; the HTTP test is intermittent (`dev docs: vbs-enclave/TESTING.md`).  
⁹ SGX: without the quote step of the check, which needs remote attestation (`RA_TYPE=dcap`, the AESM service and a PCCS).  
¹⁰ Direct riscv64: native run with `test/direct/test-direct.sh`, not `exec.sh`. Examples not named in that script were not run (–).  
¹¹ Needs `libevent` (`libevent-dev`).  
¹² The `multi-thread` and `networks` suites of `test-direct.sh`.  
