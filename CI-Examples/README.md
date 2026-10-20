# Example Integrations

This folder contains application integration examples. Each subdirectory has a `README.md` with
instructions to build and run the application under Gramine.

# Running an example

Install the backend first (`Documentation/quickstart/`). Each example has an `exec.sh` that builds
the example and runs it under one backend. Run it in the directory of the example:

```sh
./exec.sh --vm        # also --direct and --sev (--backend B is the same as --B)
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

Servers (`curl`, `iperf`, `lighttpd`, `memcached`, `mongodb`, `nginx`, `redis`, `rust`, `python`)
run under the backend, and a client on the host checks them through the loopback; the `vm` and `sev`
relay forwards the server's port (`GRAMINE_VM_INBOUND`). `curl` serves its page on an address of the
host that is not loopback, because in a `vm` or `sev` guest `127.0.0.1` is the guest itself.
`unit_tests` runs each test of its subdirectories (`align_test` is not a test). The shared code is
`common_tools/exec-common.sh`; its header lists the variables an `exec.sh` sets.

Not run by `exec.sh`: `blender` (needs a scene data set), `pytorch` (needs a model download),
`graphics` (needs a display), `ra-tls/*` (SGX attestation only) and `vbs/*` (Windows VBS only).

# Results

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
