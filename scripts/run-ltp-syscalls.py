#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-or-later
"""Run selected LTP syscall tests under native Linux or a Gramine loader."""

import argparse
import concurrent.futures
import configparser
import json
import os
import pathlib
import re
import signal
import shlex
import shutil
import subprocess
import sys
import time
import traceback

try:
    import psutil
except ImportError:
    psutil = None

ROOT = pathlib.Path(__file__).resolve().parents[1]
STATUS = {"TPASS": "passed", "PASS": "passed", "TFAIL": "failed", "FAIL": "failed",
          "TCONF": "skipped", "CONF": "skipped", "TBROK": "broken", "BROK": "broken"}


def ltp_test_command(command):
    """Return the final test command, accounting for LTP scenario pipelines."""
    return command[command.index("|") + 1] if "|" in command else command[0]


def build_argv(command, loader, binary):
    """Build an argv for a test, including the occasional LTP shell pipeline."""
    loader_program = {"direct": "gramine-direct", "vm": "gramine-vm"}.get(loader)
    if "|" not in command:
        if loader == "native":
            return [str(binary), *command[1:]]
        return [loader_program, *command]

    segments, current = [], []
    for token in command:
        if token == "|":
            segments.append(current)
            current = []
        else:
            current.append(token)
    segments.append(current)
    shell_parts = []
    for segment in segments:
        if segment and segment[0] == binary.name:
            if loader == "native":
                segment = [str(binary), *segment[1:]]
            else:
                segment = [loader_program, *segment]
        shell_parts.append(shlex.join(segment))
    return ["/bin/bash", "-c", " | ".join(shell_parts)]


def parse_output(output):
    """Parse old numbered TPASS/FAIL lines and tst_test Summary blocks."""
    subtests = []
    for line in output.splitlines():
        match = re.search(r"\b(TPASS|TFAIL|TCONF|TBROK|PASS|FAIL|CONF|BROK):?\b", line)
        if not match:
            continue
        token = match.group(1)
        before = line[:match.start()].split()
        number = next((int(value) for value in reversed(before) if value.isdigit()), None)
        subtests.append({"number": number, "result": token, "message": line.strip()})

    summary = {}
    in_summary = False
    for line in output.splitlines():
        if re.match(r"^\s*Summary\s*:?\s*$", line):
            in_summary = True
            continue
        if in_summary:
            match = re.match(r"^\s*(passed|failed|broken|skipped)\s*[: ]+\s*(\d+)\s*$", line)
            if match:
                summary[match.group(1)] = int(match.group(2))
    counts = {key: 0 for key in ("passed", "failed", "broken", "skipped")}
    for test in subtests:
        counts[STATUS[test["result"]]] += 1
    # Summary totals are authoritative for tst_test output, which may summarize repeats,
    # cleanup checks, or results that are not printed as one status line each.
    counts.update(summary)
    return subtests, counts


def classify_result(loader, returncode, timed_out, counts, output):
    if timed_out:
        return "timeout"
    if loader == "vm" and (returncode == 134 or "VM exited with code 134" in output
                           or "unhandled exception" in output and "Panic" in output):
        return "guest_panic"
    if counts["failed"]:
        return "fail"
    if counts["broken"]:
        return "broken"
    if counts["passed"] == 0 and counts["skipped"] and returncode in (0, 32):
        return "conf"
    if returncode != 0:
        return "fail"
    return "pass"


def parser_self_test():
    old_api = "getpid01    1  TPASS  :  getpid returned 31689"
    new_api = """tst_test.c:1248: TINFO: Timeout per run is 0h 05m 00s
creat01.c:50: TPASS: creat() truncated file to 0 bytes
creat01.c:55: TPASS: file was created and written to successfully
creat01.c:60: TPASS: read failed expectedly: EBADF (9)

Summary:
passed   6
failed   0
skipped  0
warnings 0"""
    old_subtests, old_counts = parse_output(old_api)
    new_subtests, new_counts = parse_output(new_api)
    assert old_counts["passed"] == 1 and len(old_subtests) == 1
    assert new_counts["passed"] == 6 and new_counts["failed"] == 0 and len(new_subtests) == 3
    legacy_lines, legacy_counts = parse_output("""case01 1 PASS: old API passed
case01 2 FAIL: old API failed""")
    summary_lines, summary_counts = parse_output("""Summary:
passed 2
failed 1
broken 3
skipped 4""")
    assert len(legacy_lines) == 2 and legacy_counts["passed"] == 1 and legacy_counts["failed"] == 1
    assert summary_counts == {"passed": 2, "failed": 1, "broken": 3, "skipped": 4}
    assert classify_result("vm", 134, False, {"failed": 0, "broken": 0, "passed": 0, "skipped": 0}, "") == "guest_panic"
    assert classify_result("vm", -15, True, {"failed": 0, "broken": 0, "passed": 0, "skipped": 0}, "") == "timeout"
    assert classify_result("vm", 32, False, {"failed": 0, "broken": 0, "passed": 0, "skipped": 1}, "") == "conf"
    print("parser self-test passed: real old-API getpid01/new-API creat01 and PASS/FAIL Summary fixtures")


def read_scenario(path):
    tests = {}
    for line in path.read_text().splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        fields = shlex.split(line)
        tests[fields[0]] = fields[1:]
    return tests


def kill_process_group(process):
    try:
        os.killpg(process.pid, signal.SIGTERM)
    except ProcessLookupError:
        return
    try:
        process.wait(timeout=3)
    except subprocess.TimeoutExpired:
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        process.wait()


def partial_output(timeout_error):
    output = timeout_error.output or ""
    return output.decode(errors="replace") if isinstance(output, bytes) else output


def find_guest(cid):
    """Find QEMU processes for the guest CID reserved by this runner."""
    if psutil is None:
        return [], None
    qemu_pids = []
    for child in psutil.process_iter(["pid", "name", "cmdline"]):
        try:
            cmdline = " ".join(child.info["cmdline"] or [])
            if "qemu" not in child.info["name"] or f"guest-cid={cid}" not in cmdline:
                continue
            qemu_pids.append(child.info["pid"])
        except (psutil.Error, ProcessLookupError):
            continue
    return qemu_pids, cid if qemu_pids else None


def allocate_guest_cids(count):
    """Reserve unique CIDs from this container's range before starting parallel guests."""
    if psutil is None:
        raise RuntimeError("VM runs require psutil to identify and clean up the runner's QEMU")
    used = set()
    for process in psutil.process_iter(["name", "cmdline"]):
        try:
            if "qemu" not in process.info["name"]:
                continue
            match = re.search(r"guest-cid=(\d+)", " ".join(process.info["cmdline"] or []))
            if match:
                used.add(int(match.group(1)))
        except psutil.Error:
            continue
    base = int(os.environ.get("GRAMINE_VM_ID_BASE", "10"))
    result = []
    cid = base
    while len(result) < count:
        if cid > 2 and cid < 0xFFFFFFFF and cid not in used:
            result.append(cid)
            used.add(cid)
        cid += 1
    return result


def stop_guest(process, qemu_pids):
    """Stop this test's QEMU by PID, then let its launcher stop relay and virtiofsd."""
    for pid in qemu_pids:
        try:
            os.kill(pid, signal.SIGTERM)
        except ProcessLookupError:
            pass
    deadline = time.monotonic() + 2
    def qemu_is_live(pid):
        if not psutil:
            return False
        try:
            return psutil.Process(pid).status() != psutil.STATUS_ZOMBIE
        except psutil.Error:
            return False

    while psutil and time.monotonic() < deadline and any(qemu_is_live(pid) for pid in qemu_pids):
        time.sleep(0.05)
    for pid in qemu_pids:
        if qemu_is_live(pid):
            try:
                os.kill(pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
    try:
        process.send_signal(signal.SIGTERM)
        process.wait(timeout=3)
    except subprocess.TimeoutExpired:
        kill_process_group(process)
    except ProcessLookupError:
        pass


def remove_guest_files(cid):
    """Remove only this run's stale virtiofs files after its QEMU is gone."""
    if cid is None:
        return
    paths = [pathlib.Path(f"/tmp/gramine_vhostfs_{cid}"),
             pathlib.Path(f"/tmp/gramine_vhostfs_{cid}.pid"),
             pathlib.Path(f"/tmp/gramine_vm_{cid}.lock")]
    if psutil:
        for process in psutil.process_iter(["cmdline"]):
            try:
                if "qemu" in process.name() and f"guest-cid={cid}" in " ".join(process.info["cmdline"] or []):
                    return
            except psutil.Error:
                continue
    for path in paths:
        if path.is_dir() and not path.is_symlink():
            shutil.rmtree(path, ignore_errors=True)
        else:
            path.unlink(missing_ok=True)


def kill_test_orphans(binary, existing_pids):
    """Kill exact LTP executable PIDs left behind after their parent times out."""
    if psutil is None:
        return
    target = binary.resolve()
    pids = []
    for process in psutil.process_iter(["pid", "cmdline"]):
        try:
            if process.info["pid"] in existing_pids:
                continue
            argv = process.info["cmdline"] or []
            if argv and pathlib.Path(argv[0]).resolve() == target:
                pids.append(process.info["pid"])
        except (psutil.Error, OSError):
            continue
    for pid in pids:
        try:
            os.kill(pid, signal.SIGTERM)
        except ProcessLookupError:
            pass
    if pids:
        time.sleep(0.1)
    for pid in pids:
        try:
            os.kill(pid, signal.SIGKILL)
        except ProcessLookupError:
            pass


def run_one(name, command, args, per_test_timeout, reserved_cid=None):
    binary = pathlib.Path(args.ltp_root) / "testcases/bin" / ltp_test_command(command)
    env = os.environ.copy()
    env["PATH"] = f"{binary.parent}:{env.get('PATH', '/usr/bin:/bin')}"
    if args.loader == "vm":
        env["GRAMINE_VM_ID_BASE"] = str(reserved_cid)
    argv = build_argv(command, args.loader, binary)
    existing_pids = set(psutil.pids()) if psutil else set()
    start = time.monotonic()
    proc = subprocess.Popen(argv, cwd=args.harness_dir, env=env, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True, errors="replace",
                            start_new_session=True)
    timed_out = False
    qemu_pids, cid = [], reserved_cid
    try:
        if args.loader == "vm":
            deadline = time.monotonic() + min(2, per_test_timeout)
            while proc.poll() is None and time.monotonic() < deadline and cid is None:
                qemu_pids, found_cid = find_guest(reserved_cid)
                if found_cid is None:
                    time.sleep(0.02)
        output, _ = proc.communicate(timeout=per_test_timeout)
    except subprocess.TimeoutExpired as timeout_error:
        timed_out = True
        if args.loader == "vm":
            qemu_pids, _ = find_guest(reserved_cid)
            stop_guest(proc, qemu_pids)
        else:
            kill_process_group(proc)
            kill_test_orphans(binary, existing_pids)
        output = partial_output(timeout_error)
        # A test may leave a child in uninterruptible kernel sleep. It can retain the pipe after
        # the process group is signalled; closing our read end prevents that child from blocking
        # the runner forever. The timeout output remains available for the failure report.
        if proc.stdout:
            proc.stdout.close()
    except BaseException:
        if proc.poll() is None:
            if args.loader == "vm":
                qemu_pids, _ = find_guest(reserved_cid)
                stop_guest(proc, qemu_pids)
            else:
                kill_process_group(proc)
                kill_test_orphans(binary, existing_pids)
        raise
    if args.loader == "vm":
        remove_guest_files(cid)
    duration = time.monotonic() - start
    subtests, counts = parse_output(output)
    returncode = proc.returncode
    status = classify_result(args.loader, returncode, timed_out, counts, output)
    panic = status == "guest_panic"
    result = {"loader": args.loader, "test": name, "subtests": subtests, "counts": counts,
              "exit_status": returncode, "status": status, "duration_seconds": round(duration, 3),
              "timeout": timed_out, "timeout_seconds": per_test_timeout, "guest_panic": panic,
              "guest_cid": cid}
    if status != "pass":
        result["output_tail"] = output.splitlines()[-20:]
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--loader", choices=("native", "direct", "vm"))
    parser.add_argument("--scenario", type=pathlib.Path)
    parser.add_argument("--ltp-root", type=pathlib.Path, default=ROOT / ".ltp-install")
    parser.add_argument("--harness-dir", type=pathlib.Path, default=ROOT / "libos/test/ltp")
    parser.add_argument("--config", type=pathlib.Path)
    parser.add_argument("--results", type=pathlib.Path)
    parser.add_argument("--timeout", type=float, help="override every per-test timeout")
    parser.add_argument("--timeout-factor", type=float, default=1.0)
    parser.add_argument("--jobs", type=int, default=1)
    parser.add_argument("--skip-test", action="append", default=[], metavar="NAME",
                        help="record NAME as TCONF without executing it")
    parser.add_argument("--skip-reason", default="excluded by --skip-test")
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("tests", nargs="*")
    args = parser.parse_args()
    if args.self_test:
        parser_self_test()
        return
    if not args.loader:
        parser.error("--loader is required")
    if args.jobs < 1 or args.jobs > 4:
        parser.error("--jobs must be between 1 and 4")
    if args.loader == "vm" and args.jobs != 1:
        parser.error("--jobs 1 is required for VM runs to keep guest CID and cleanup ownership isolated")
    args.scenario = args.scenario or args.ltp_root / "runtest/syscalls"
    args.config = args.config or args.harness_dir / "ltp_all.cfg"
    config = configparser.ConfigParser(defaults={"timeout": "30"})
    config.read(args.config)
    scenarios = read_scenario(args.scenario)
    selected = args.tests or list(scenarios)
    unknown = sorted(set(selected) - set(scenarios))
    if unknown:
        parser.error("tests missing from scenario: " + ", ".join(unknown))
    unknown_skips = sorted(set(args.skip_test) - set(selected))
    if unknown_skips:
        parser.error("skipped tests missing from selection: " + ", ".join(unknown_skips))
    active = [name for name in selected if name not in args.skip_test]
    if args.loader != "native":
        missing = sorted({ltp_test_command(scenarios[name]) for name in active
                          if not (args.harness_dir / f"{ltp_test_command(scenarios[name])}.manifest").exists()})
        if missing:
            parser.error("manifests missing; build them with gramine-test build first: " + ", ".join(missing[:10]))
    guest_cids = allocate_guest_cids(1) * len(active) if args.loader == "vm" else [None] * len(active)

    results = [{"loader": args.loader, "test": name, "subtests": [],
                "counts": {"passed": 0, "failed": 0, "skipped": 1, "broken": 0},
                "exit_status": None, "status": "conf", "duration_seconds": 0,
                "timeout": False, "guest_panic": False, "skip_reason": args.skip_reason}
               for name in selected if name in args.skip_test]
    timeout_by_test = {
        name: (args.timeout if args.timeout is not None else
               config.get(name, "timeout", fallback="30", raw=True))
        for name in active
    }
    timeout_by_test = {name: float(timeout) * args.timeout_factor
                       for name, timeout in timeout_by_test.items()}
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        tasks = {pool.submit(run_one, name, scenarios[name], args, timeout_by_test[name], cid): name
                 for name, cid in zip(active, guest_cids)}
        for future in concurrent.futures.as_completed(tasks):
            name = tasks[future]
            try:
                result = future.result()
            except Exception as error:
                result = {"loader": args.loader, "test": name, "subtests": [],
                          "counts": {key: 0 for key in ("passed", "failed", "skipped", "broken")},
                          "exit_status": None, "status": "runner_error", "duration_seconds": None,
                          "timeout": False, "guest_panic": False,
                          "runner_error": f"{error!r}\n{traceback.format_exc()}"}
            results.append(result)
            duration = result.get("duration_seconds")
            duration_text = f"{duration:.2f}s" if duration is not None else "n/a"
            print(f"{result['loader']} {result['test']}: {result['status']} ({duration_text})", flush=True)
    results.sort(key=lambda result: selected.index(result["test"]))
    out = {"loader": args.loader, "results": results}
    if args.results:
        args.results.parent.mkdir(parents=True, exist_ok=True)
        args.results.write_text(json.dumps(out, indent=2) + "\n")
    totals = {status: sum(result["status"] == status for result in results)
              for status in ("pass", "fail", "conf", "broken", "timeout", "guest_panic", "runner_error")}
    print(json.dumps({"tests": len(results), "totals": totals, "results_file": str(args.results) if args.results else None}))


if __name__ == "__main__":
    main()
