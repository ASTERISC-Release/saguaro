# Example Integrations

This folder contains application integration examples. For instructions how to build and 
run each application under Gramine, please see the README.md in each subdirectory.

Note that most of the examples use oversimplified configurations which
are not secure (e.g., they mount whole host directories into the application).
If you take these examples as templates for your own workloads, please inspect
and harden the configurations.

| Example | Direct x86_64 | VM x86_64 | Direct aarch64 | VM aarch64 | SEV | VBS |
|---|---|---|---|---|---|---|
| `bash` | - | - | - | - | - | not run: no VBS recipe |
| `blender` | - | - | - | - | - | not run: no VBS recipe |
| `busybox` | - | - | - | - | - | pass (ARM64, x64) |
| `curl` | - | - | - | - | - | pass (ARM64, x64) |
| `gcc` | - | - | - | - | - | not run: no VBS recipe |
| `graphics` | - | - | - | - | - | not run: no VBS recipe |
| `helloworld` | - | - | - | - | - | pass (ARM64, x64) |
| `iperf` | - | - | - | - | - | pass (ARM64, x64) |
| `lighttpd` | - | - | - | - | - | pass (ARM64, x64) |
| `memcached` | - | - | - | - | - | pass (ARM64, x64) |
| `mongodb` | - | - | - | - | - | not run: no VBS recipe (x18 in mongod) |
| `nginx` | - | - | - | - | - | pass (ARM64, x64) |
| `nodejs` | - | - | - | - | - | fail on ARM64 (x18 in the node binary and libstdc++); not run on x64 (the recipe packs the linux-arm64 node) |
| `openjdk` | - | - | - | - | - | not run: no VBS recipe (the JVM needs executable memory) |
| `python` | - | - | - | - | - | ARM64: pass (hello and HTTP tests; NumPy/SciPy not packed); x64: pass (4/4 with NumPy and SciPy; the HTTP test 2/4 fails in about one run in five) |
| `pytorch` | - | - | - | - | - | not run: no VBS recipe |
| `r` | - | - | - | - | - | not run: no VBS recipe |
| `ra-tls-mbedtls` | - | - | - | - | - | not run: SGX attestation example |
| `ra-tls-nginx` | - | - | - | - | - | not run: SGX attestation example |
| `ra-tls-secret-prov` | - | - | - | - | - | not run: SGX attestation example |
| `redis` | - | - | - | - | - | pass (ARM64, x64) |
| `rust` | - | - | - | - | - | pass (ARM64, x64; needs rustup stable with rust-src) |
| `sqlite` | - | - | - | - | - | pass (ARM64, x64) |
| `unit_tests` | - | - | - | - | - | not run: LibOS/PAL test programs, no VBS recipe |
| `vbs/attestation` | - | - | - | - | - | pass (ARM64, x64) |
| `vbs/dynhello` | - | - | - | - | - | pass (ARM64, x64) |
| `vbs/helloworld` | - | - | - | - | - | pass (ARM64, x64) |
| `vbs/python` | - | - | - | - | - | pass (ARM64, x64) |
| `vbs/tcpserver` | - | - | - | - | - | pass (ARM64, x64) |
| `vbs/tls-echo` | - | - | - | - | - | pass (ARM64, x64) |
