#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-or-later
"""Rows of the syscall matrix with the VM's experimental multiprocess support.

Reads the published per-test matrix (`dev docs: syscall-compat/matrix.csv`, which gives
the native, direct and VM columns of `MATRIX.md`) and one LTP run of the stock install under
`gramine-vm` with `sys.experimental__multiprocess = true` (`scripts/run-ltp-syscalls.py --loader
vm` on manifests built with `LTP_VM_MULTIPROCESS=1`), and writes a Markdown table: the loader
totals, then one row per syscall of the process, signal and IPC groups and per other syscall whose
key-on result differs from the VM no-fork column. `MATRIX.md` itself is not changed: the key is
experimental (see `../syscall-compat/RERUN.md`).
"""

import argparse
import collections
import csv
import json
import os
import pathlib

ROOT = pathlib.Path(__file__).resolve().parents[1]
# The syscall-compat and syscall-profile data live in the private development-documents repository, checked out
# next to this one (GRAMINE_DEV_DOCS names another place).
DEV_DOCS = pathlib.Path(os.environ.get("GRAMINE_DEV_DOCS", ROOT.parent / "gramine-multiarch-docs"))
COMPAT = DEV_DOCS / "syscall-compat"
STATUSES = ("pass", "fail", "conf", "broken", "timeout", "guest_panic", "runner_error")
LETTERS = dict(zip(STATUSES, "PFCBTXE"))
GROUPS = ("process", "signal", "ipc")


def counts(statuses):
    counter = collections.Counter(statuses)
    return " ".join(f"{LETTERS[s]}{counter[s]}" for s in STATUSES if counter[s]) or "—"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--matrix", type=pathlib.Path, default=COMPAT / "matrix.csv")
    parser.add_argument("--syscalls", type=pathlib.Path, default=COMPAT / "syscalls.csv")
    parser.add_argument("--results", type=pathlib.Path,
                        default=ROOT / ".ltp-results/vm-multiprocess.json")
    parser.add_argument("--output", type=pathlib.Path)
    args = parser.parse_args()

    tests = {row["test"]: row for row in csv.DictReader(args.matrix.open())}
    groups = {row["syscall"]: row["group"] for row in csv.DictReader(args.syscalls.open())}
    keyon = {row["test"]: row["status"] for row in json.loads(args.results.read_text())["results"]}

    by_syscall = collections.defaultdict(list)
    for name, row in tests.items():
        for call in filter(None, row["syscalls"].split(";")):
            by_syscall[call].append(name)

    lines = ["| Loader | " + " | ".join(LETTERS[s] for s in STATUSES) + " |",
             "|---|" + "---:|" * len(STATUSES)]
    for label, column in (("native", "native"), ("direct", "direct"),
                          ("VM no-fork", "vm_nofork")):
        counter = collections.Counter(row[column] for row in tests.values())
        lines.append(f"| {label} | " + " | ".join(str(counter[s]) for s in STATUSES) + " |")
    counter = collections.Counter(keyon.get(name, "absent") for name in tests)
    lines.append("| VM key on (experimental) | " + " | ".join(str(counter[s]) for s in STATUSES)
                 + " |")
    absent = counter["absent"]
    lines += ["", f"{len(tests)} tests in the matrix; {absent} of them are not in the key-on run.",
              "", "| Syscall | Group | LTP tests | Native | Direct | VM no-fork | VM key on |",
              "|---|---|---:|---|---|---|---|"]
    for call in sorted(by_syscall, key=lambda c: (groups.get(c, "~"), c)):
        names = by_syscall[call]
        nofork = [tests[n]["vm_nofork"] for n in names]
        on = [keyon.get(n, "absent") for n in names]
        if groups.get(call) not in GROUPS and sum(s == "pass" for s in on) == \
                sum(s == "pass" for s in nofork):
            continue
        lines.append(f"| {call} | {groups.get(call, '?')} | {len(names)} | "
                     f"{counts(tests[n]['native'] for n in names)} | "
                     f"{counts(tests[n]['direct'] for n in names)} | {counts(nofork)} | "
                     f"{counts(s for s in on if s != 'absent')} |")
    text = "\n".join(lines) + "\n"
    if args.output:
        args.output.write_text(text)
    else:
        print(text, end="")


if __name__ == "__main__":
    main()
