#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The ASTERISC Research Lab
"""Run the LTP syscall tests of one loader as N parallel shards of scripts/run-ltp-syscalls.py,
merge the shard results into one results file, and optionally rerun every non-pass serially.

    scripts/ltp-sharded.py --loader direct --shards 8 --results .ltp-results/direct.json \\
        [--serial-rerun] [--ltp-root DIR] [--harness-dir DIR] -- [runner arguments and tests]
    scripts/ltp-sharded.py merge --results OUT SHARD.json...

Shard K of N runs the tests whose index i in the selection has i % N == K - 1. Each shard is one
runner process with its own process group, its own results file and log, its own TMPDIR (under
/tmp, mode 1777) and, for the vm loader, its own guest CID (SAGUARO_VM_ID_BASE + 2 (K - 1));
native and direct shards run in their own IPC, UTS and network namespaces (`unshare`, loopback
up) when the container allows it.
The harness (install symlink, /dev/shm mounts) is prepared once before the shards start; shards
only check it. The merged file has the shape of a serial results file (scope order, `loader`,
`ltp_root`) plus the shard count, commit and wall time; shards of different loaders, installs,
scenarios or commits are refused.

With --serial-rerun every test that did not pass (and was not skipped by --skip-test) is run
again by one serial runner process (one test at a time, one guest, no namespaces). The merged
file then holds the serial row of each rerun test, with the parallel result kept under
`parallel`; the tests whose status differs are listed under `serial_rerun.changed` and printed.
See dev docs: syscall-compat/RERUN.md.
"""

import argparse
import json
import os
import pathlib
import shutil
import signal
import subprocess
import sys
import time

try:
    import psutil
except ImportError:
    psutil = None

ROOT = pathlib.Path(__file__).resolve().parents[1]
RUNNER = ROOT / "scripts/run-ltp-syscalls.py"
TOKEN_VAR = "LTP_RUNNER_TOKEN"   # set by the runner in each test's environment
STATUSES = ("pass", "fail", "conf", "broken", "timeout", "guest_panic", "runner_error")
CHECKED_KEYS = ("loader", "ltp_root", "scenario", "commit", "scope_count")


def merge_shards(shards):
    """Join shard result dicts into one serial-shaped result dict, in scope order."""
    if not shards:
        raise ValueError("no shard files")
    first = shards[0]
    for key in CHECKED_KEYS:
        values = {json.dumps(shard.get(key)) for shard in shards}
        if len(values) != 1:
            raise ValueError(f"shards differ in {key}: {', '.join(sorted(values))}")
    if not first.get("shard"):
        raise ValueError("not a shard file (no 'shard' key); run the runner with --shard K/N")
    count = int(first["shard"].split("/")[1])
    by_index = {}
    for shard in shards:
        index, total = (int(value) for value in shard["shard"].split("/"))
        if total != count or index in by_index:
            raise ValueError(f"shard {shard['shard']} is duplicated or of another split")
        by_index[index] = shard["results"]
    if sorted(by_index) != list(range(1, count + 1)):
        raise ValueError(f"missing shards: have {sorted(by_index)} of {count}")
    scope = first["scope_count"]
    results = []
    for i in range(scope):
        shard_results = by_index[i % count + 1]
        position = i // count
        if position >= len(shard_results):
            raise ValueError(f"shard {i % count + 1} has {len(shard_results)} results, too few")
        results.append(shard_results[position])
    if sum(len(rows) for rows in by_index.values()) != scope:
        raise ValueError("shards hold more results than the scope")
    names = [row["test"] for row in results]
    if len(set(names)) != len(names):
        raise ValueError("a test appears in two shards")
    return {"loader": first["loader"], "ltp_root": first["ltp_root"], "scenario": first["scenario"],
            "commit": first["commit"], "shards": count, "results": results}


def merge_self_test():
    def shard(k, n, names):
        return {"loader": "native", "ltp_root": "/l", "scenario": "/s", "commit": "c",
                "scope_count": 5, "shard": f"{k}/{n}",
                "results": [{"test": name, "status": "pass"} for name in names]}
    merged = merge_shards([shard(2, 2, ["b", "d"]), shard(1, 2, ["a", "c", "e"])])
    assert [row["test"] for row in merged["results"]] == ["a", "b", "c", "d", "e"]
    for bad in ([shard(1, 2, ["a", "c", "e"])],
                [shard(1, 2, ["a", "c", "e"]), dict(shard(2, 2, ["b", "d"]), ltp_root="/other")],
                [shard(1, 2, ["a", "c", "e"]), dict(shard(2, 2, ["b", "d"]), commit="x")]):
        try:
            merge_shards(bad)
        except ValueError:
            continue
        raise AssertionError("bad shard set was merged")
    print("merge self-test passed: interleaved order, missing shard, mixed install, mixed commit")


def namespaces_usable():
    """True when `unshare` can give a process its own IPC, UTS and network namespaces here."""
    if not shutil.which("unshare") or not shutil.which("ip"):
        return False
    probe = subprocess.run(["unshare", "--ipc", "--uts", "--net", "--", "ip", "link", "set", "lo", "up"],
                           capture_output=True)
    return probe.returncode == 0


def kill_tree_of(runner_pid):
    """Kill the tests (and their guests, relays, virtiofsd) started by one runner process: they
    carry the runner's PID in the token of their environment, also after setsid()."""
    if psutil is None:
        return
    prefix = f"{runner_pid}:"
    victims = []
    for process in psutil.process_iter(["pid"]):
        try:
            if process.environ().get(TOKEN_VAR, "").startswith(prefix):
                victims.append(process)
        except (psutil.Error, OSError):
            continue
    for process in victims:
        try:
            process.kill()
        except psutil.Error:
            pass


def summarize(results):
    return {status: sum(row["status"] == status for row in results) for status in STATUSES}


def run_shards(args, runner_args, workdir):
    base = int(os.environ.get("SAGUARO_VM_ID_BASE", "10"))
    isolate = args.loader != "vm" and args.isolate == "auto" and namespaces_usable()
    if args.loader != "vm" and args.isolate == "auto" and not isolate:
        print("note: unshare --ipc --uts --net is not usable here; shards share those namespaces",
              file=sys.stderr)
    shards = []
    # TMPDIRs under /tmp, world-writable and sticky like /tmp itself: tests that drop to an
    # unprivileged user (fchown03, ...) create files there, and the workdir is under /root
    tmp_base = pathlib.Path("/tmp") / f"ltp-sharded-{args.loader}-{os.getpid()}"
    shutil.rmtree(tmp_base, ignore_errors=True)
    tmp_base.mkdir(mode=0o755)
    for k in range(1, args.shards + 1):
        tmpdir = tmp_base / f"shard-{k}"
        tmpdir.mkdir()
        tmpdir.chmod(0o1777)
        out = workdir / f"shard-{k}.json"
        out.unlink(missing_ok=True)
        env = os.environ.copy()
        env["TMPDIR"] = str(tmpdir)
        if args.loader == "vm":
            env["SAGUARO_VM_ID_BASE"] = str(base + 2 * (k - 1))
        command = [sys.executable, str(RUNNER), "--loader", args.loader, "--ltp-root", str(args.ltp_root),
                   "--harness-dir", str(args.harness_dir), "--shard", f"{k}/{args.shards}",
                   "--results", str(out), *runner_args]
        if isolate:
            command = ["unshare", "--ipc", "--uts", "--net", "--", "sh", "-c",
                       'ip link set lo up && exec "$@"', "sh", *command]
        log = (workdir / f"shard-{k}.log").open("w")
        process = subprocess.Popen(command, cwd=ROOT, env=env, stdout=log, stderr=subprocess.STDOUT,
                                   start_new_session=True)
        shards.append((k, process, out, log))
    print(f"started {args.shards} {args.loader} shards (logs in {workdir}); "
          f"namespaces: {'ipc+uts+net' if isolate else 'shared'}", flush=True)
    return shards, isolate


def wait_shards(shards, load):
    done = {}
    try:
        while len(done) < len(shards):
            for k, process, _, _ in shards:
                if k not in done and process.poll() is not None:
                    done[k] = (process.returncode, time.monotonic())
            load["max_load1"] = max(load["max_load1"], os.getloadavg()[0])
            if psutil:
                load["min_available_gib"] = min(load["min_available_gib"],
                                                psutil.virtual_memory().available / 2**30)
            time.sleep(1)
    except BaseException:
        for _, process, _, _ in shards:
            if process.poll() is None:
                try:
                    os.killpg(process.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
            # the runner process (inside `unshare sh -c exec`, same PID) started the tests
            kill_tree_of(process.pid)
        raise
    finally:
        for _, _, _, log in shards:
            log.close()
    return done


def serial_rerun(args, runner_args, merged, workdir):
    skipped = set(args.skip_names)
    names = [row["test"] for row in merged["results"] if row["status"] != "pass"
             and row["test"] not in skipped and not row.get("skip_reason")]
    if not names:
        return {"tests": 0, "changed": [], "wall_seconds": 0}
    out = workdir / "serial.json"
    out.unlink(missing_ok=True)
    passthrough = [arg for arg in runner_args if arg not in args.tests]
    command = [sys.executable, str(RUNNER), "--loader", args.loader, "--ltp-root", str(args.ltp_root),
               "--harness-dir", str(args.harness_dir), "--results", str(out),
               *strip_skips(passthrough), *names]
    print(f"serial rerun of {len(names)} non-pass {args.loader} tests", flush=True)
    start = time.monotonic()
    with (workdir / "serial.log").open("w") as log:
        rc = subprocess.run(command, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT).returncode
    wall = time.monotonic() - start
    if rc != 0 or not out.exists():
        raise RuntimeError(f"serial rerun failed (exit {rc}); see {workdir / 'serial.log'}")
    serial = {row["test"]: row for row in json.loads(out.read_text())["results"]}
    changed = []
    for index, row in enumerate(merged["results"]):
        new = serial.get(row["test"])
        if new is None:
            continue
        new = dict(new)
        new["parallel"] = {key: row.get(key) for key in
                           ("status", "exit_status", "counts", "duration_seconds", "timeout",
                            "guest_panic", "guest_cid", "output_tail")}
        new["serial_rerun"] = True
        if new["status"] != row["status"]:
            changed.append({"test": row["test"], "parallel": row["status"], "serial": new["status"]})
        merged["results"][index] = new
    return {"tests": len(names), "changed": changed, "wall_seconds": round(wall, 1)}


def strip_skips(arguments):
    """Drop --skip-test/--skip-reason (and their values): the rerun names its tests explicitly."""
    result, skip_next = [], False
    for argument in arguments:
        if skip_next:
            skip_next = False
            continue
        if argument in ("--skip-test", "--skip-reason"):
            skip_next = True
            continue
        if argument.startswith(("--skip-test=", "--skip-reason=")):
            continue
        result.append(argument)
    return result


def main():
    if len(sys.argv) > 1 and sys.argv[1] == "merge":
        parser = argparse.ArgumentParser(prog="ltp-sharded.py merge")
        parser.add_argument("--results", type=pathlib.Path, required=True)
        parser.add_argument("shard_files", nargs="+", type=pathlib.Path)
        args = parser.parse_args(sys.argv[2:])
        merged = merge_shards([json.loads(path.read_text()) for path in args.shard_files])
        args.results.write_text(json.dumps(merged, indent=2) + "\n")
        print(json.dumps({"tests": len(merged["results"]), "totals": summarize(merged["results"])}))
        return
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--loader", choices=("native", "direct", "vm"))
    parser.add_argument("--shards", type=int, default=8)
    parser.add_argument("--results", type=pathlib.Path)
    parser.add_argument("--ltp-root", type=pathlib.Path, default=ROOT / ".ltp-install")
    parser.add_argument("--harness-dir", type=pathlib.Path, default=ROOT / "libos/test/ltp")
    parser.add_argument("--workdir", type=pathlib.Path,
                        help="shard results, logs and TMPDIRs (default: <results>.shards/)")
    parser.add_argument("--isolate", choices=("auto", "none"), default="auto",
                        help="auto: own IPC/UTS/network namespaces for native and direct shards")
    parser.add_argument("--serial-rerun", action="store_true",
                        help="rerun every non-pass test serially and keep both results")
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("runner_args", nargs="*",
                        help="after --: arguments for run-ltp-syscalls.py (timeouts, skips, tests)")
    args = parser.parse_args()
    if args.self_test:
        merge_self_test()
        return
    if not args.loader or not args.results:
        parser.error("--loader and --results are required")
    if not 1 <= args.shards <= 18:
        parser.error("--shards must be between 1 and 18")
    if args.loader == "vm" and psutil is None:
        parser.error("vm shards need psutil (python3-psutil) for guest cleanup")
    runner_args = args.runner_args
    for forbidden in ("--shard", "--results", "--loader", "--ltp-root", "--harness-dir", "--jobs"):
        if any(arg == forbidden or arg.startswith(forbidden + "=") for arg in runner_args):
            parser.error(f"{forbidden} is set by this driver")
    args.skip_names = [runner_args[i + 1] for i, arg in enumerate(runner_args[:-1]) if arg == "--skip-test"]
    values = {runner_args[i + 1] for i, arg in enumerate(runner_args[:-1])
              if arg.startswith("--") and "=" not in arg and arg not in ("--self-test",)}
    args.tests = [arg for arg in runner_args if not arg.startswith("--") and arg not in values]
    args.ltp_root = args.ltp_root.resolve()
    args.harness_dir = args.harness_dir.resolve()
    workdir = (args.workdir or args.results.with_name(args.results.name + ".shards")).resolve()
    workdir.mkdir(parents=True, exist_ok=True)

    prepare = subprocess.run([sys.executable, str(RUNNER), "--loader", args.loader,
                              "--ltp-root", str(args.ltp_root), "--harness-dir", str(args.harness_dir),
                              "--prepare-only", *runner_args], cwd=ROOT)
    if prepare.returncode != 0:
        sys.exit("preparing the harness failed")

    load = {"start_load1": os.getloadavg()[0], "max_load1": 0.0,
            "min_available_gib": float("inf")}
    start = time.monotonic()
    shards, isolate = run_shards(args, runner_args, workdir)
    done = wait_shards(shards, load)
    wall = time.monotonic() - start
    shutil.rmtree(pathlib.Path("/tmp") / f"ltp-sharded-{args.loader}-{os.getpid()}", ignore_errors=True)
    failed = {k: rc for k, (rc, _) in done.items() if rc != 0}
    if failed:
        sys.exit(f"shards failed: {failed}; see the logs in {workdir}")
    merged = merge_shards([json.loads(out.read_text()) for _, _, out, _ in shards])
    merged.update({
        "wall_seconds": round(wall, 1),
        "shard_wall_seconds": {str(k): round(t - start, 1) for k, (_, t) in sorted(done.items())},
        "test_seconds": round(sum(row.get("duration_seconds") or 0 for row in merged["results"]), 1),
        "namespaces": "ipc+uts+net" if isolate else "shared",
        "cpu_count": os.cpu_count(),
        "load": {key: (round(value, 2) if value != float("inf") else None) for key, value in load.items()},
    })
    totals = summarize(merged["results"])
    print(json.dumps({"tests": len(merged["results"]), "parallel_totals": totals,
                      "wall_seconds": merged["wall_seconds"], "test_seconds": merged["test_seconds"]}),
          flush=True)
    if args.serial_rerun:
        merged["parallel_totals"] = totals
        merged["serial_rerun"] = serial_rerun(args, runner_args, merged, workdir)
        for change in merged["serial_rerun"]["changed"]:
            print(f"differs: {change['test']}: parallel {change['parallel']}, serial {change['serial']}")
    args.results.parent.mkdir(parents=True, exist_ok=True)
    args.results.write_text(json.dumps(merged, indent=2) + "\n")
    print(json.dumps({"tests": len(merged["results"]), "totals": summarize(merged["results"]),
                      "results_file": str(args.results)}))


if __name__ == "__main__":
    main()
