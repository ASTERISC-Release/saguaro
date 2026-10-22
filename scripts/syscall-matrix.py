#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-or-later
"""Join the syscall inventory, LTP mapping, and measured loader results.

`--arch x86_64` (the default) writes MATRIX.md, matrix.csv and syscalls.csv from the x86_64
results in .ltp-results/; `--arch aarch64` writes MATRIX-aarch64.md, matrix-aarch64.csv and
syscalls-aarch64.csv from .ltp-results/aarch64/, with the AArch64 LibOS states of inventory.csv
and a comparison with the committed x86_64 matrix.csv.

A loader whose JSON file is absent (the native and VM unmodified runs, which are not rerun with
every update and whose JSON is not kept in the repository) is read from the committed per-test
CSV instead: status, exit code and, for native failures, the evidence text; its run metadata
(shards, wall times, serial reruns) comes from recorded-runs.toml. Pass --native etc. to read a
fresh JSON instead."""

import argparse
import collections
import csv
import json
import os
import pathlib
import re
import tomllib


ROOT = pathlib.Path(__file__).resolve().parents[1]
# The syscall-compat and syscall-profile data live in the private development-documents repository, checked out
# next to this one (SAGUARO_DEV_DOCS names another place).
DEV_DOCS = pathlib.Path(os.environ.get("SAGUARO_DEV_DOCS", ROOT.parent / "saguaro-docs"))
TOPIC = DEV_DOCS / "syscall-compat"
RESULTS = ROOT / ".ltp-results"
STATUSES = ("pass", "fail", "conf", "broken", "timeout", "guest_panic", "runner_error")
REASONS = ("by design", "LibOS gap", "behaviour difference", "test environment", "crash or hang")
GROUPS = ("file", "memory", "process", "signal", "socket", "ipc", "time", "misc")


def run_seconds(results):
    return round(sum(row.get("duration_seconds") or 0 for row in results.values()))


def read_results(path):
    data = json.loads(path.read_text())
    rows = {row["test"]: row for row in data["results"]}
    if len(rows) != len(data["results"]):
        raise ValueError(f"duplicate test in {path}")
    for row in rows.values():
        if row["status"] not in STATUSES:
            raise ValueError(f"unknown status {row['status']} in {path}")
    return rows

def run_meta(data):
    """The run metadata of a results file (or of recorded-runs.toml) that the documents print."""
    return {field: data.get(field) for field in
            ("shards", "wall_seconds", "test_seconds", "commit", "serial_rerun")
            if data.get(field) is not None}


def recorded_results(csv_path, key, meta):
    """Per-test results of one loader taken from the committed CSV (see the module docstring)."""
    rows = {}
    parallel = {}
    prefix = "Native container did not pass: "
    for row in csv.DictReader(csv_path.open()):
        entry = {"test": row["test"], "status": row[key], "exit_status": int(row[key + "_exit"]) if row[key + "_exit"] else None,
                 "subtests": [], "output_tail": []}
        if key == "native" and row[key] != "pass" and row["evidence"].startswith(prefix):
            entry["subtests"] = [{"result": "TFAIL", "message": row["evidence"][len(prefix):]}]
        for item in filter(None, row.get("parallel", "").split(";")):
            loader, change = item.split(":")
            if loader == key:
                entry["parallel"] = {"status": change.split(">")[0]}
        rows[row["test"]] = entry
    for row in rows.values():  # only the sum of the durations is printed
        row["duration_seconds"] = meta.get("run_seconds", 0) / len(rows)
    return rows


def failure_message(row):
    messages = [sub["message"] for sub in row.get("subtests", [])
                if sub["result"] in ("TFAIL", "TBROK", "TCONF")]
    if not messages:
        messages = [line for line in row.get("output_tail", [])
                    if any(term in line for term in ("Panic", "ENOSYS", "error:", "failed"))]
    return (messages or row.get("output_tail", [])[-1:] or [""])[0]


def combined_output(row):
    return "\n".join([sub["message"] for sub in row.get("subtests", [])] + row.get("output_tail", []))


def is_process_design_failure(message, syscalls):
    lower = message.lower()
    if any(call in ("fork", "vfork", "execve", "clone3") for call in syscalls):
        return True
    if "clone" in syscalls and re.search(r"clone.{0,80}(enosys|failed|not implemented)", lower):
        return True
    return bool(re.search(r"(?:fork|vfork|exec(?:ve)?|process creation).{0,90}"
                          r"(?:enosys|failed|not implemented|could not|unsupported)", lower)
                or re.search(r"(?:could not|cannot|can't).{0,30}fork", lower))


def environment_hint(message):
    lower = message.lower()
    return any(term in lower for term in (
        "requires kernel", "requires root", "needs root", "kernel config", "not supported on your arch",
        "couldn't find 'useradd'", "failed to find module", "requires libaio", "no test device",
        "not enough free"))


def classify(name, syscalls, inventory, native, direct, vm, reviewed, state_key="x86_64_state"):
    n, d, v = native[name], direct[name], vm[name]
    if n["status"] != "pass":
        return "test environment", "Native container did not pass: " + failure_message(n)
    if name in reviewed and v["status"] != "pass" and reviewed[name].get("applies", True):
        entry = reviewed[name]
        return entry["reason"], entry["evidence"]
    vm_message = combined_output(v)
    if v["status"] != "pass" and is_process_design_failure(vm_message, syscalls):
        return "by design", failure_message(v)
    if v["status"] != "pass" and ("/sys/" in vm_message or
                                       ("/dev/" in vm_message and
                                        re.search(r"ENOSYS|EBADF|not supported", vm_message))):
        return "by design", failure_message(v)
    if v["status"] == "pass" and d["status"] == "pass":
        return "", ""
    if any(inventory[call][state_key] in ("ENOSYS", "stub")
           for call in syscalls if call in inventory):
        return "LibOS gap", failure_message(v if v["status"] != "pass" else d)
    if d["status"] != "pass" and re.search(r"ENOSYS|not implemented|not supported", combined_output(d), re.I):
        return "LibOS gap", failure_message(d)
    if d["status"] != "pass":
        return "behaviour difference", failure_message(d)
    if v["status"] in ("timeout", "guest_panic") or v.get("exit_status") in (137, 139):
        return "crash or hang", failure_message(v) or f"VM exited {v.get('exit_status')}"
    if v["status"] != "pass" and environment_hint(vm_message):
        return "test environment", failure_message(v)
    if v["status"] != "pass" and re.search(r"ENOSYS|not implemented|syscall\(\d+\).*not supported", vm_message, re.I):
        return "LibOS gap", failure_message(v)
    return "behaviour difference", failure_message(v if v["status"] != "pass" else d)


def compact(message, limit=140):
    cleaned = re.sub(r"\s+", " ", message).replace("|", "/").replace("`", "'").strip()
    return cleaned[:limit - 1] + "…" if len(cleaned) > limit else cleaned


def status_counts(tests, results):
    counts = collections.Counter(results[name]["status"] for name in tests)
    labels = (("pass", "P"), ("fail", "F"), ("conf", "C"), ("broken", "B"),
              ("timeout", "T"), ("guest_panic", "X"), ("runner_error", "E"))
    return " ".join(f"{letter}{counts[status]}" for status, letter in labels if counts[status]) or "—"


def vm_status(tests, vm, reasons):
    if not tests:
        return "untested"
    passing = sum(vm[name]["status"] == "pass" for name in tests)
    if passing == len(tests):
        return "full"
    if passing:
        return "partial"
    if all(reasons[name][0] == "by design" for name in tests):
        return "n/a by design"
    return "none"


def markdown(inventory, by_syscall, native, direct, original_vm, vm, reasons, mapped_count, unmapped_count,
             aarch64=None, runs=None):
    """The matrix document. `aarch64` (a dict, see aarch64_context()) selects the AArch64 variant;
    without it the x86_64 document is written exactly as before the AArch64 mode existed."""
    all_tests = list(native)
    if aarch64:
        lines = aarch64_intro(native, original_vm, vm, mapped_count, unmapped_count, aarch64)
    else:
        lines = x86_64_intro(all_tests, original_vm, vm, mapped_count, unmapped_count, runs)
    lines += ["## Loader totals", "", "| Loader | P | F | C | B | T | X | E |",
              "|---|---:|---:|---:|---:|---:|---:|---:|"]
    for label, result in (("native", native), ("direct", direct), ("VM unmodified", original_vm),
                          ("VM no-fork", vm)):
        counts = collections.Counter(row["status"] for row in result.values())
        lines.append("| " + label + " | " + " | ".join(str(counts[status]) for status in STATUSES) + " |")
    lines += ["", "## Test reason totals", "", "| Reason | Tests |", "|---|---:|"]
    reason_counts = collections.Counter(reason for reason, _ in reasons.values() if reason)
    for reason in REASONS:
        lines.append(f"| {reason} | {reason_counts[reason]} |")
    vm_nonpass = sum(vm[name]["status"] != "pass" for name in all_tests)
    lines += ["", f"Of the VM no-fork tests, {vm_nonpass} did not pass. Their reasons are:", "",
              "| VM non-pass reason | Tests |", "|---|---:|"]
    vm_reason_counts = collections.Counter(reasons[name][0] for name in all_tests
                                           if vm[name]["status"] != "pass")
    for reason in REASONS:
        lines.append(f"| {reason} | {vm_reason_counts[reason]} |")
    if aarch64:
        lines += aarch64_review_text(aarch64)
    else:
        lines += ["", "A reason is assigned to each test with a non-pass result. Native non-pass "
                  "takes precedence as a container test-environment limitation. Otherwise the "
                  "VM no-fork result is compared with direct. The syscall row shows the most "
                  "frequent reason among its non-passing tests; all reasons remain in `matrix.csv`. "
                  "Thirty source-and-output reviews are recorded in `classification.toml`."]
    lines += ["", "## Inventory by group and source state", "",
              "| Group | x86_64 implemented / stub / ENOSYS | AArch64 implemented / stub / ENOSYS |",
              "|---|---:|---:|"]
    for group in GROUPS:
        row = [f"| {group} "]
        for arch in ("x86_64", "aarch64"):
            count = collections.Counter(item[f"{arch}_state"] for item in inventory.values()
                                        if item["group"] == group and item[f"{arch}_number"])
            row.append(" / ".join(str(count[state]) for state in ("implemented", "stub", "ENOSYS")))
        lines.append("| " + " | ".join([group, *row[1:]]) + " |")
    totals = []
    for arch in ("x86_64", "aarch64"):
        count = collections.Counter(item[f"{arch}_state"] for item in inventory.values()
                                    if item[f"{arch}_number"])
        totals.append(" / ".join(str(count[state]) for state in ("implemented", "stub", "ENOSYS")))
    lines.append("| **total** | " + " | ".join(totals) + " |")
    if aarch64:
        lines += aarch64_comparison(native, direct, original_vm, vm, aarch64)
        lines += ["", "## Syscall matrix", "",
                  "| Syscall | AArch64 no. | x86_64 no. | Group | AArch64 LibOS | x86_64 LibOS | LTP tests | Native | Direct | VM unmodified | VM no-fork | VM status | Main reason | Example |",
                  "|---|---:|---:|---|---|---|---:|---|---|---|---|---|---|---|"]
    else:
        lines += ["", "## Syscall matrix", "",
                  "| Syscall | x86_64 no. | AArch64 no. | Group | x86_64 LibOS | AArch64 LibOS | LTP tests | Native | Direct | VM unmodified | VM no-fork | VM status | Main reason | Example |",
                  "|---|---:|---:|---|---|---|---:|---|---|---|---|---|---|---|"]
    for name, item in inventory.items():
        tests = by_syscall[name]
        status = vm_status(tests, vm, reasons)
        failures = [test for test in tests if reasons[test][0]]
        main = "—"
        example = "—"
        if failures:
            counter = collections.Counter(reasons[test][0] for test in failures)
            main = sorted(counter, key=lambda reason: (-counter[reason], REASONS.index(reason)))[0]
            test = next(test for test in failures if reasons[test][0] == main)
            example = f"{test}: {compact(reasons[test][1], 95)}"
        numbers = (item["x86_64_number"] or "—", item["aarch64_number"] or "—")
        states = (item["x86_64_state"], item["aarch64_state"])
        if aarch64:
            numbers, states = numbers[::-1], states[::-1]
        values = (name, *numbers, item["group"], *states, str(len(tests)),
                  status_counts(tests, native), status_counts(tests, direct),
                  status_counts(tests, original_vm), status_counts(tests, vm),
                  status, main, example)
        lines.append("| " + " | ".join(values) + " |")
    lines.append("")
    return "\n".join(lines)


def x86_64_intro(all_tests, original_vm, vm, mapped_count, unmapped_count, runs):
    shards = sorted({str(meta["shards"]) for meta in runs.values() if "shards" in meta})
    commits = sorted({meta["commit"] for meta in runs.values() if "commit" in meta})
    changed = sum(len(meta.get("serial_rerun", {}).get("changed", [])) for meta in runs.values())
    if shards:
        measured = ("The direct and VM no-fork columns ran as " + "/".join(shards) + " parallel "
                    "shards (`scripts/ltp-sharded.py`; VM shards one guest with one vCPU each), "
                    "then every non-pass test was run again serially; the matrix uses the serial "
                    f"result, and {changed} tests changed between the two. They were measured at "
                    "commit " + ", ".join(commits) + ". The native and unmodified VM columns are "
                    "earlier serial runs (one worker; VM one guest and one vCPU) that were not "
                    "repeated.")
    else:
        measured = ("All four loaders used one worker; VM runs used one guest and one vCPU.")
    return [
        "# Syscall compatibility of `gramine-vm` (x86_64)", "",
        "Generated by `scripts/syscall-matrix.py` from `inventory.csv`, `mapping.toml`, "
        "`classification.toml` and the saved LTP runner JSON (the native and unmodified VM "
        "columns from the previous `matrix.csv`, see `recorded-runs.toml`). See "
        "[matrix.csv](matrix.csv) for every test and its reason, and [RERUN.md](RERUN.md) for "
        "the commands.", "",
        "The numbers are Linux UAPI syscall numbers. LibOS state comes from source inspection; "
        "the LTP results are x86_64 (the AArch64 results are in "
        "[MATRIX-aarch64.md](MATRIX-aarch64.md)). Both VM columns use a VM tmpfs `/dev/shm`; "
        "the no-fork column uses the compile-time LTP shim. The per-test results for both are "
        "in `matrix.csv`.", "",
        "`P` pass, `F` fail, `C` configuration skip, `B` broken setup, `T` timeout, "
        "`X` guest panic, `E` runner error. Counts are test binaries, not individual TPASS "
        "subtests. A test mapped to several syscalls contributes to each of those rows. "
        "`full` means all mapped tests pass, `partial` means some pass, `none` means none pass, "
        "`n/a by design` means every mapped test is blocked by a documented VM design limit, "
        "and `untested` means no mapped LTP test.", "",
        f"Scope: {len(all_tests)} scenario entries, {mapped_count} mapped and {unmapped_count} "
        "unmapped. `signal06` is an explicit TCONF because it could signal container PID 1. "
        + measured + " The unmodified "
        "VM column runs the stock LTP build, whose library forks before every test (the VM PAL "
        "cannot fork, so those tests break), the no-fork column the shim build. The test "
        f"durations sum to {run_seconds(original_vm):,} seconds for the unmodified VM run and "
        f"{run_seconds(vm):,} seconds for the no-fork VM run.", "",
    ]


LOADERS = (("native", "native"), ("direct", "direct"), ("vm_unmodified", "VM unmodified"),
           ("vm_nofork", "VM no-fork"))


def aarch64_intro(native, original_vm, vm, mapped_count, unmapped_count, ctx):
    runs = ctx["runs"]
    shards = sorted({str(meta.get("shards")) for meta in runs.values()})
    changed = sum(len(meta.get("serial_rerun", {}).get("changed", [])) for meta in runs.values())
    return [
        "# Syscall compatibility of `gramine-vm` (AArch64)", "",
        "Generated by `scripts/syscall-matrix.py --arch aarch64` from `inventory.csv`, "
        "`mapping.toml`, `classification.toml`, `classification-aarch64.toml`, the saved AArch64 "
        "LTP runner JSON and, for the comparison, the x86_64 [matrix.csv](matrix.csv). See "
        "[matrix-aarch64.csv](matrix-aarch64.csv) for every test, its reason and its x86_64 "
        "results, [MATRIX.md](MATRIX.md) for the x86_64 matrix and [RERUN.md](RERUN.md) for the "
        "commands.", "",
        "The numbers are Linux UAPI syscall numbers; the AArch64 columns come first. AArch64 has no "
        "number for the legacy calls that the generic syscall table dropped (`open`, `stat`, "
        "`fork`, ...): their LTP tests use the newer call or report TCONF. LibOS state comes from "
        "source inspection of the AArch64 syscall table. The runs were made on an AArch64 machine "
        "with KVM (see `TESTING.md` for the host and the conditions). The AArch64 VM PAL is "
        "single-process (no `fork`/`exec`), like the x86_64 VM columns; the native and unmodified "
        "VM runs (2026-10-15) were built from their commit plus `aarch64-vm-build.patch`, a "
        "build fix for the AArch64 VM PAL (see `TODO.md`), the direct and no-fork runs "
        "(2026-10-17, commit 17405cd3) need no patch. Both VM columns use a VM tmpfs `/dev/shm`; the no-fork column uses the "
        "compile-time LTP shim.", "",
        "`P` pass, `F` fail, `C` configuration skip, `B` broken setup, `T` timeout, "
        "`X` guest panic, `E` runner error. Counts are test binaries, not individual TPASS "
        "subtests. A test mapped to several syscalls contributes to each of those rows. "
        "`full` means all mapped tests pass, `partial` means some pass, `none` means none pass, "
        "`n/a by design` means every mapped test is blocked by a documented VM design limit, "
        "and `untested` means no mapped LTP test. As on x86_64, a test that exits 0 without a "
        "result line counts as a pass: `modify_ldt01` to `modify_ldt03` only print "
        "\"test only for ix86\" on AArch64. Exit 134 counts as a guest panic even when it is the "
        "test's own abort (`timer_delete01`).", "",
        f"Scope: {len(native)} scenario entries, {mapped_count} mapped and {unmapped_count} "
        "unmapped. `signal06` is an explicit TCONF under the Gramine loaders because it could "
        "signal container PID 1. Each loader ran as " + "/".join(shards) + " parallel shards "
        "(`scripts/ltp-sharded.py`; VM shards one guest with one vCPU each), then every non-pass "
        "test was run again serially (one test at a time, one guest); the matrix uses the serial "
        f"result, and {changed} tests changed between the two (listed under parallel runs below "
        "and in the `parallel` column of the CSV). The test durations sum to "
        f"{run_seconds(original_vm):,} seconds for the unmodified VM run and {run_seconds(vm):,} "
        "seconds for the no-fork VM run.", "",
    ]


def aarch64_review_text(ctx):
    return ["", "A reason is assigned to each test with a non-pass result, by the rules of the "
            "x86_64 matrix: native non-pass first (test environment), then documented VM design "
            "limits, LibOS gaps visible under direct, VM-only behaviour differences and hangs. "
            f"{ctx['x86_reused']} of the {ctx['x86_reviews']} x86_64 reviews of "
            "`classification.toml` are reused (only where the AArch64 VM no-fork status equals "
            "the x86_64 one); "
            f"{ctx['aarch64_reviewed']} tests were reviewed by hand for AArch64 "
            "(`classification-aarch64.toml`). Every other reason is automatic and marked "
            "`unreviewed` in the `review` column of `matrix-aarch64.csv`."]


def aarch64_comparison(native, direct, original_vm, vm, ctx):
    x86 = ctx["x86"]
    lines = ["", "## Parallel runs", "",
             "Wall times of the sharded pass and of the serial rerun; the test time is the sum of "
             "the per-test durations of the sharded pass (about what one serial run would take).",
             "",
             "| Loader | Shards | Sharded wall (s) | Test time (s) | Serial reruns | Rerun wall (s) | "
             "Changed |", "|---|---:|---:|---:|---:|---:|---:|"]
    for key, label in LOADERS:
        meta = ctx["runs"].get(key, {})
        rerun = meta.get("serial_rerun", {})
        lines.append(f"| {label} | {meta.get('shards', '—')} | {meta.get('wall_seconds', '—')} | "
                     f"{meta.get('test_seconds', '—')} | {rerun.get('tests', '—')} | "
                     f"{rerun.get('wall_seconds', '—')} | {len(rerun.get('changed', []))} |")
    changes = [f"{label}: " + ", ".join(f"`{c['test']}` {c['parallel']}→{c['serial']}"
                                        for c in ctx["runs"].get(key, {}).get("serial_rerun", {})
                                        .get("changed", []))
               for key, label in LOADERS if ctx["runs"].get(key, {}).get("serial_rerun", {}).get("changed")]
    if changes:
        lines += ["", "Parallel → serial status of the tests that changed:", ""]
        lines += [f"- {change}" for change in changes]
    lines += ["", "## Compared with x86_64", "",
              "Per loader, the tests by where they pass (x86_64 from `matrix.csv`).", "",
              "| Loader | Pass on both | x86_64 only | AArch64 only | Neither |",
              "|---|---:|---:|---:|---:|"]
    lists = []
    for (key, label), result in zip(LOADERS, (native, direct, original_vm, vm)):
        both = x86_only = a64_only = neither = 0
        only = []
        for name, row in result.items():
            a = row["status"] == "pass"
            b = x86.get(name, {}).get(key) == "pass"
            both += a and b
            neither += not a and not b
            a64_only += a and not b
            if b and not a:
                x86_only += 1
                only.append(f"`{name}` ({row['status']})")
        lines.append(f"| {label} | {both} | {x86_only} | {a64_only} | {neither} |")
        if only:
            lists.append(f"- {label}: " + ", ".join(only))
    if lists:
        lines += ["", "Tests that pass on x86_64 and not on AArch64 (AArch64 status in brackets):", ""]
        lines += lists
    return lines


def direct_status(tests, direct):
    if not tests:
        return "untested"
    passing = sum(direct[name]["status"] == "pass" for name in tests)
    return "full" if passing == len(tests) else "partial" if passing else "none"


def write_syscalls_csv(path, inventory, by_syscall, direct, vm, reasons):
    """One row per syscall, for scripts/syscall-profile.py."""
    fieldnames = ("syscall", "x86_64_number", "aarch64_number", "group", "x86_64_state",
                  "aarch64_state", "ltp_tests", "direct_pass", "direct_status", "vm_pass",
                  "vm_status", "vm_main_reason", "vm_failing_tests")
    with path.open("w", newline="") as output:
        writer = csv.DictWriter(output, fieldnames=fieldnames, lineterminator="\n")
        writer.writeheader()
        for name, item in inventory.items():
            tests = by_syscall[name]
            failures = [test for test in tests if reasons[test][0]]
            counter = collections.Counter(reasons[test][0] for test in failures)
            main = (sorted(counter, key=lambda reason: (-counter[reason], REASONS.index(reason)))[0]
                    if failures else "")
            writer.writerow({
                "syscall": name, "x86_64_number": item["x86_64_number"],
                "aarch64_number": item["aarch64_number"], "group": item["group"],
                "x86_64_state": item["x86_64_state"], "aarch64_state": item["aarch64_state"],
                "ltp_tests": len(tests),
                "direct_pass": sum(direct[t]["status"] == "pass" for t in tests),
                "direct_status": direct_status(tests, direct),
                "vm_pass": sum(vm[t]["status"] == "pass" for t in tests),
                "vm_status": vm_status(tests, vm, reasons), "vm_main_reason": main,
                "vm_failing_tests": ";".join(t for t in tests if vm[t]["status"] != "pass")})


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--arch", choices=("x86_64", "aarch64"), default="x86_64")
    parser.add_argument("--inventory", type=pathlib.Path, default=TOPIC / "inventory.csv")
    parser.add_argument("--mapping", type=pathlib.Path, default=TOPIC / "mapping.toml")
    parser.add_argument("--classification", type=pathlib.Path, default=TOPIC / "classification.toml")
    parser.add_argument("--classification-aarch64", type=pathlib.Path,
                        default=TOPIC / "classification-aarch64.toml",
                        help="--arch aarch64: AArch64 hand reviews (optional file)")
    parser.add_argument("--x86-matrix", type=pathlib.Path, default=TOPIC / "matrix.csv",
                        help="--arch aarch64: the x86_64 per-test results to compare with")
    parser.add_argument("--native", type=pathlib.Path)
    parser.add_argument("--direct", type=pathlib.Path)
    parser.add_argument("--vm-unmodified", type=pathlib.Path)
    parser.add_argument("--vm", type=pathlib.Path)
    parser.add_argument("--recorded-runs", type=pathlib.Path, default=TOPIC / "recorded-runs.toml",
                        help="metadata of the loaders that are read from the CSV (no JSON given)")
    parser.add_argument("--output-md", type=pathlib.Path)
    parser.add_argument("--output-csv", type=pathlib.Path)
    parser.add_argument("--output-syscalls", type=pathlib.Path)
    args = parser.parse_args()
    aarch64 = args.arch == "aarch64"
    results_dir = RESULTS / "aarch64" if aarch64 else RESULTS
    suffix = "-aarch64" if aarch64 else ""
    args.native = args.native or results_dir / "native.json"
    args.direct = args.direct or results_dir / "direct.json"
    args.vm_unmodified = args.vm_unmodified or results_dir / "vm-unmodified-tmpfs.json"
    args.vm = args.vm or results_dir / "vm-nofork-tmpfs.json"
    args.output_md = args.output_md or TOPIC / f"MATRIX{suffix}.md"
    args.output_csv = args.output_csv or TOPIC / f"matrix{suffix}.csv"
    args.output_syscalls = args.output_syscalls or TOPIC / f"syscalls{suffix}.csv"
    inventory = {row["syscall"]: row for row in csv.DictReader(args.inventory.open())}
    mapping_data = tomllib.loads(args.mapping.read_text())
    mapping = {row["name"]: row["syscalls"] for row in mapping_data["test"]}
    unmapped = set(mapping_data["unmapped"]["tests"])
    reviewed_data = tomllib.loads(args.classification.read_text())["reviewed"]
    reviewed = {row["name"]: row for row in reviewed_data}
    if len(reviewed) != len(reviewed_data) or len(reviewed) < 30:
        raise ValueError("the hand-reviewed sample needs at least 30 unique tests")
    if any(row["reason"] not in REASONS for row in reviewed.values()):
        raise ValueError("reviewed case has an unknown reason")
    recorded = tomllib.loads(args.recorded_runs.read_text()).get(args.arch, {}) \
        if args.recorded_runs.exists() else {}
    runs, loaded = {}, []
    for key, path in (("native", args.native), ("direct", args.direct),
                      ("vm_unmodified", args.vm_unmodified), ("vm_nofork", args.vm)):
        if path.exists():
            loaded.append(read_results(path))
            runs[key] = run_meta(json.loads(path.read_text()))
        elif key in recorded:
            loaded.append(recorded_results(args.output_csv, key, recorded[key]))
            runs[key] = dict(run_meta(recorded[key]), recorded=True)
        else:
            raise FileNotFoundError(f"{path}: no JSON and no recorded run for {key}")
    native, direct, original_vm, vm = loaded
    tests = set(native)
    if any(set(result) != tests for result in (direct, original_vm, vm)):
        raise ValueError("loader result sets have different test names")
    if tests != set(mapping) | unmapped or set(mapping) & unmapped:
        raise ValueError("mapping and result-set scope differ")
    unknown_calls = sorted({call for calls in mapping.values() for call in calls if call not in inventory})
    if unknown_calls:
        raise ValueError("mapped syscall absent from inventory: " + ", ".join(unknown_calls))
    by_syscall = {name: [] for name in inventory}
    for test, calls in mapping.items():
        for call in calls:
            by_syscall[call].append(test)
    context = None
    state_key = "x86_64_state"
    if aarch64:
        state_key = "aarch64_state"
        context, reviewed = aarch64_context(args, reviewed, vm, runs)
    reasons = {name: classify(name, mapping.get(name, []), inventory, native, direct, vm, reviewed,
                              state_key)
               for name in sorted(tests)}
    if any(not reason and any(result[name]["status"] != "pass" for result in (native, direct, vm))
           for name, (reason, _) in reasons.items()):
        raise ValueError("a non-pass test has no reason")
    fieldnames = ("test", "syscalls", "native", "direct", "vm_unmodified", "vm_nofork",
                  "native_exit", "direct_exit", "vm_unmodified_exit", "vm_nofork_exit",
                  "vm_nofork_passed", "vm_nofork_failed", "vm_nofork_skipped", "vm_nofork_broken",
                  "reason", "evidence")
    if aarch64:
        fieldnames += ("review", "parallel", "x86_64_native", "x86_64_direct", "x86_64_vm_unmodified",
                       "x86_64_vm_nofork")
    args.output_csv.parent.mkdir(parents=True, exist_ok=True)
    with args.output_csv.open("w", newline="") as output:
        writer = csv.DictWriter(output, fieldnames=fieldnames, lineterminator="\n")
        writer.writeheader()
        for name in sorted(tests):
            counts = vm[name]["counts"]
            row = {"test": name, "syscalls": ";".join(mapping.get(name, [])),
                   "native": native[name]["status"], "direct": direct[name]["status"],
                   "vm_unmodified": original_vm[name]["status"], "vm_nofork": vm[name]["status"],
                   "native_exit": native[name]["exit_status"],
                   "direct_exit": direct[name]["exit_status"],
                   "vm_unmodified_exit": original_vm[name]["exit_status"],
                   "vm_nofork_exit": vm[name]["exit_status"],
                   "vm_nofork_passed": counts["passed"], "vm_nofork_failed": counts["failed"],
                   "vm_nofork_skipped": counts["skipped"], "vm_nofork_broken": counts["broken"],
                   "reason": reasons[name][0], "evidence": compact(reasons[name][1], 250)}
            if aarch64:
                row.update(aarch64_csv_columns(name, reasons[name][0], native, vm, reviewed,
                                               (native, direct, original_vm, vm), context))
            writer.writerow(row)
    write_syscalls_csv(args.output_syscalls, inventory, by_syscall, direct, vm, reasons)
    args.output_md.write_text(markdown(inventory, by_syscall, native, direct, original_vm, vm,
                                       reasons, len(mapping), len(unmapped), context, runs))
    print(f"wrote {len(inventory)} syscalls and {len(tests)} tests; "
          f"reason totals: {dict(collections.Counter(reason for reason, _ in reasons.values() if reason))}")


def aarch64_context(args, reviewed, vm, runs):
    """The x86_64 results, the run metadata and the reviews that apply on AArch64: an x86_64
    review only where the AArch64 VM no-fork status equals the x86_64 one, an AArch64 review
    (classification-aarch64.toml) always."""
    x86 = {row["test"]: row for row in csv.DictReader(args.x86_matrix.open())}
    combined = {}
    for name, entry in reviewed.items():
        same = name in vm and x86.get(name, {}).get("vm_nofork") == vm[name]["status"]
        combined[name] = dict(entry, applies=same, origin="x86_64")
    aarch64_reviewed = 0
    if args.classification_aarch64.exists():
        rows = tomllib.loads(args.classification_aarch64.read_text()).get("reviewed", [])
        for row in rows:
            if row["reason"] not in REASONS:
                raise ValueError(f"AArch64 review of {row['name']} has an unknown reason")
            combined[row["name"]] = dict(row, applies=True, origin="aarch64")
        aarch64_reviewed = len(rows)
    reused = sum(1 for entry in combined.values() if entry["origin"] == "x86_64" and entry["applies"])
    return {"x86": x86, "runs": runs, "x86_reused": reused, "x86_reviews": len(reviewed),
            "aarch64_reviewed": aarch64_reviewed}, combined


def aarch64_csv_columns(name, reason, native, vm, reviewed, results, ctx):
    review = ""
    if reason:
        entry = reviewed.get(name)
        if native[name]["status"] != "pass":
            review = "native non-pass"
        elif entry and entry["applies"] and vm[name]["status"] != "pass":
            review = f"{entry['origin']} review"
        else:
            review = "unreviewed"
    parallel = []
    for (key, _), result in zip(LOADERS, results):
        row = result[name]
        if row.get("parallel") and row["parallel"]["status"] != row["status"]:
            parallel.append(f"{key}:{row['parallel']['status']}>{row['status']}")
    x86 = ctx["x86"].get(name, {})
    return {"review": review, "parallel": ";".join(parallel),
            "x86_64_native": x86.get("native", ""), "x86_64_direct": x86.get("direct", ""),
            "x86_64_vm_unmodified": x86.get("vm_unmodified", ""),
            "x86_64_vm_nofork": x86.get("vm_nofork", "")}


if __name__ == "__main__":
    main()
