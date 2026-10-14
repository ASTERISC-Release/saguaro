#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-or-later
"""Join the syscall inventory, LTP mapping, and measured loader results."""

import argparse
import collections
import csv
import json
import pathlib
import re
import tomllib


ROOT = pathlib.Path(__file__).resolve().parents[1]
TOPIC = ROOT / "Documentation/devel/syscall-compat"
RESULTS = ROOT / ".ltp-results"
STATUSES = ("pass", "fail", "conf", "broken", "timeout", "guest_panic", "runner_error")
REASONS = ("by design", "LibOS gap", "behaviour difference", "test environment", "crash or hang")
GROUPS = ("file", "memory", "process", "signal", "socket", "ipc", "time", "misc")


def read_results(path):
    data = json.loads(path.read_text())
    rows = {row["test"]: row for row in data["results"]}
    if len(rows) != len(data["results"]):
        raise ValueError(f"duplicate test in {path}")
    for row in rows.values():
        if row["status"] not in STATUSES:
            raise ValueError(f"unknown status {row['status']} in {path}")
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


def classify(name, syscalls, inventory, native, direct, vm, reviewed):
    n, d, v = native[name], direct[name], vm[name]
    if n["status"] != "pass":
        return "test environment", "Native container did not pass: " + failure_message(n)
    if name in reviewed and v["status"] != "pass":
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
    if any(inventory[call]["x86_64_state"] in ("ENOSYS", "stub")
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


def markdown(inventory, by_syscall, native, direct, original_vm, vm, reasons, mapped_count, unmapped_count):
    all_tests = list(native)
    lines = [
        "# Syscall compatibility of `gramine-vm` (x86_64)", "",
        "Generated by `scripts/syscall-matrix.py` from `inventory.csv`, `mapping.toml`, "
        "`classification.toml` and the saved LTP runner JSON. See [matrix.csv](matrix.csv) "
        "for every test and its reason, and [RERUN.md](RERUN.md) for the commands.", "",
        "The numbers are Linux UAPI syscall numbers. LibOS state comes from source inspection; "
        "only x86_64 has measured LTP results. AArch64 numbers and source state are inventory "
        "only. Both VM columns use a VM tmpfs `/dev/shm`; the no-fork column uses the "
        "compile-time LTP shim. The per-test results for both are in `matrix.csv`.", "",
        "`P` pass, `F` fail, `C` configuration skip, `B` broken setup, `T` timeout, "
        "`X` guest panic, `E` runner error. Counts are test binaries, not individual TPASS "
        "subtests. A test mapped to several syscalls contributes to each of those rows. "
        "`full` means all mapped tests pass, `partial` means some pass, `none` means none pass, "
        "`n/a by design` means every mapped test is blocked by a documented VM design limit, "
        "and `untested` means no mapped LTP test.", "",
        f"Scope: {len(all_tests)} scenario entries, {mapped_count} mapped and {unmapped_count} "
        "unmapped. `signal06` is an explicit TCONF because it could signal container PID 1. "
        "All four loaders used one worker; VM runs used one guest and one vCPU. The corrected "
        "unmodified VM run took 2,402 seconds; the no-fork VM run took 2,395 seconds.", "",
        "## Loader totals", "", "| Loader | P | F | C | B | T | X | E |", "|---|---:|---:|---:|---:|---:|---:|---:|"
    ]
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
    lines += ["", "A reason is assigned to each test with a non-pass result. Native non-pass "
              "takes precedence as a container test-environment limitation. Otherwise the "
              "VM no-fork result is compared with direct. The syscall row shows the most "
              "frequent reason among its non-passing tests; all reasons remain in `matrix.csv`. "
              "Thirty source-and-output reviews are recorded in `classification.toml`.", "",
              "## Inventory by group and source state", "",
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
        values = (name, item["x86_64_number"] or "—", item["aarch64_number"] or "—",
                  item["group"], item["x86_64_state"], item["aarch64_state"], str(len(tests)),
                  status_counts(tests, native), status_counts(tests, direct),
                  status_counts(tests, original_vm), status_counts(tests, vm),
                  status, main, example)
        lines.append("| " + " | ".join(values) + " |")
    lines.append("")
    return "\n".join(lines)


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
    parser.add_argument("--inventory", type=pathlib.Path, default=TOPIC / "inventory.csv")
    parser.add_argument("--mapping", type=pathlib.Path, default=TOPIC / "mapping.toml")
    parser.add_argument("--classification", type=pathlib.Path, default=TOPIC / "classification.toml")
    parser.add_argument("--native", type=pathlib.Path, default=RESULTS / "native.json")
    parser.add_argument("--direct", type=pathlib.Path, default=RESULTS / "direct.json")
    parser.add_argument("--vm-unmodified", type=pathlib.Path, default=RESULTS / "vm-unmodified-tmpfs.json")
    parser.add_argument("--vm", type=pathlib.Path, default=RESULTS / "vm-nofork-tmpfs.json")
    parser.add_argument("--output-md", type=pathlib.Path, default=TOPIC / "MATRIX.md")
    parser.add_argument("--output-csv", type=pathlib.Path, default=TOPIC / "matrix.csv")
    parser.add_argument("--output-syscalls", type=pathlib.Path, default=TOPIC / "syscalls.csv")
    args = parser.parse_args()
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
    native, direct, original_vm, vm = (read_results(path) for path in
                                       (args.native, args.direct, args.vm_unmodified, args.vm))
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
    reasons = {name: classify(name, mapping.get(name, []), inventory, native, direct, vm, reviewed)
               for name in sorted(tests)}
    if any(not reason and any(result[name]["status"] != "pass" for result in (native, direct, vm))
           for name, (reason, _) in reasons.items()):
        raise ValueError("a non-pass test has no reason")
    fieldnames = ("test", "syscalls", "native", "direct", "vm_unmodified", "vm_nofork",
                  "native_exit", "direct_exit", "vm_unmodified_exit", "vm_nofork_exit",
                  "vm_nofork_passed", "vm_nofork_failed", "vm_nofork_skipped", "vm_nofork_broken",
                  "reason", "evidence")
    args.output_csv.parent.mkdir(parents=True, exist_ok=True)
    with args.output_csv.open("w", newline="") as output:
        writer = csv.DictWriter(output, fieldnames=fieldnames, lineterminator="\n")
        writer.writeheader()
        for name in sorted(tests):
            counts = vm[name]["counts"]
            writer.writerow({"test": name, "syscalls": ";".join(mapping.get(name, [])),
                             "native": native[name]["status"], "direct": direct[name]["status"],
                             "vm_unmodified": original_vm[name]["status"], "vm_nofork": vm[name]["status"],
                             "native_exit": native[name]["exit_status"],
                             "direct_exit": direct[name]["exit_status"],
                             "vm_unmodified_exit": original_vm[name]["exit_status"],
                             "vm_nofork_exit": vm[name]["exit_status"],
                             "vm_nofork_passed": counts["passed"], "vm_nofork_failed": counts["failed"],
                             "vm_nofork_skipped": counts["skipped"], "vm_nofork_broken": counts["broken"],
                             "reason": reasons[name][0], "evidence": compact(reasons[name][1], 250)})
    write_syscalls_csv(args.output_syscalls, inventory, by_syscall, direct, vm, reasons)
    args.output_md.write_text(markdown(inventory, by_syscall, native, direct, original_vm, vm,
                                       reasons, len(mapping), len(unmapped)))
    print(f"wrote {len(inventory)} syscalls and {len(tests)} tests; "
          f"reason totals: {dict(collections.Counter(reason for reason, _ in reasons.values() if reason))}")


if __name__ == "__main__":
    main()
