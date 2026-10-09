# gramineproject/examples on aarch64 (direct PAL)

The applicable examples from <https://github.com/gramineproject/examples> (commit 9fdd89c5) now
live in `CI-Examples/`: `curl gcc iperf mongodb nodejs openjdk pytorch r`. Run them with
`scripts/examples-aarch64/run_examples.sh` (needs the Gramine install from `scripts/install.sh`).
Validated 2026-10-08 on the WSL2 aarch64 box (see `direct-pal-port-log.md`, "Validated platforms").
Not copied (x86 / Intel-specific): `tensorflow-lite`, `openvino`, `scikit-learn-intelex`.

## Host prerequisites
Every example has an idempotent `install-prereqs.sh` (Debian/Ubuntu; installs only what is missing,
uses `sudo` for apt and may prompt; `DRY_RUN=1` previews). `CI-Examples/install-all-prereqs.sh
[example...]` runs them all, and `run_examples.sh --prereqs` does that before running the tests.
- curl: curl, python3, netcat-openbsd · gcc: build-essential, patch, wget · iperf: build-essential, wget
- nodejs: nodejs · openjdk: openjdk-17 (first of 17/21/11 available) · r: r-base-core
- pytorch: python3-venv, python3-pip, lsb-release (`WITH_NSS=1` adds libnss-mdns/libnss-myhostname)
- mongodb: runtime libs, `pymongo`, and the official release tarball (SHA-256 checked) unpacked to
  `~/.local/mongodb` with `mongod` linked into `~/.local/bin` (ARMv8.2 CPU required on arm64)
`iperf` needs two processes (server under Gramine, client natively) and is not in the runner:
`cd CI-Examples/iperf && make && gramine-direct iperf3` then
`LD_LIBRARY_PATH=./install ./install/iperf3 -c 127.0.0.1`.

## Differences from upstream (portability gaps in the examples, not port bugs)
- `gcc`: the `/lib64` mount is only added when `/lib64` exists (x86-only path).
- `nodejs`, `openjdk`: a writable `tmpfs` mount at `/tmp` (the added tests write there).
- `pytorch`: installs CPU-only wheels (`--index-url https://download.pytorch.org/whl/cpu`); the
  aarch64 PyPI `torch` is the CUDA build and pulls several GB of unusable NVIDIA libraries.
- Added stress programs: `nodejs/heavy.js` (JIT hot loops, 4 worker threads, crypto/zlib, 200 HTTP
  requests, fs), `openjdk/Stress.java` (JIT, 200k deliberate NPEs = HotSpot's SIGSEGV path, GC
  pressure on 8 threads, contended `ConcurrentHashMap`, socket echo, file I/O),
  `mongodb/mongo_client_test.py` (pymongo: the example's insert/fetch plus 50k-doc bulk insert,
  indexes, aggregation, 8 concurrent writers).
- `common_tools/wait_for_server` added (used by `curl`).

## Results
| example | result |
|---|---|
| curl | pass (4/4) |
| gcc | pass (hello, bzip2, gzip compile + round-trip) |
| iperf | pass; ~native TCP/UDP throughput (needed finding #14) |
| nodejs | pass (hello 3/3; `heavy.js` 5/5, ~350 ms vs 345 ms native) |
| openjdk | pass (MultiThreadMain; `Stress` 10/10, ~480 ms vs 495 ms native) |
| r | pass (3/3) |
| pytorch | pass; output byte-identical to native; ~2.4 s vs ~2.0 s native |
| mongodb | pass (7.0.14; ~3.2 s vs 3.4 s native; data survives a restart; 5/5 cold runs) |

Notes: oneDNN prints `Can't open MIDR_EL1 sysfs entry` under Gramine (the guest does not emulate
`/sys/devices/system/cpu/cpuN/regs/identification/`; harmless). MongoDB logs benign warnings for
missing `/sys/block`, the TCP fast-open option and acceptor queue length. `gramine-direct`
resolves symlinks, so a symlinked `mongod` still uses `mongod.manifest`.
