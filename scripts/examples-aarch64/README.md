# gramineproject/examples on aarch64 (direct PAL)

Validated 2026-10-08 on the WSL2 aarch64 box (see `ARM64_MILESTONE_A.md`, "Validated platforms").
Clone <https://github.com/gramineproject/examples> next to the repo, apply
`examples-aarch64.patch` (`git apply`), then run `./run_examples.sh <examples-dir>`.

## What the patch changes (all are example portability gaps, not port bugs)
- `gcc`: the `/lib64` mount is only added when `/lib64` exists (x86-only path).
- `nodejs`, `openjdk`: add a writable `tmpfs` mount at `/tmp` (the tests below write there).
- `pytorch`: install CPU-only wheels (`--index-url https://download.pytorch.org/whl/cpu`); the
  aarch64 PyPI `torch` is the CUDA build and pulls several GB of unusable NVIDIA libraries.

## Extra stress programs (copied into the example dirs by the runner)
- `heavy.js` (Node): JIT hot loops, 4 worker threads, crypto/zlib, 200 HTTP requests, fs.
- `Stress.java` (OpenJDK 17): JIT, 200k deliberate NPEs (HotSpot's SIGSEGV path), GC pressure on 8
  threads, contended `ConcurrentHashMap`, socket echo, file I/O.

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
| tensorflow-lite, openvino, scikit-learn-intelex | not applicable (x86 / Intel-specific) |
| mongodb | not tried |

Known gap: oneDNN prints `Can't open MIDR_EL1 sysfs entry` under Gramine because the guest
`/sys/devices/system/cpu/cpuN/regs/identification/` is not emulated (host has it). Harmless.
