#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-or-later
"""Check scripts/syscall-profile.py against its own raw strace output.

For each command the raw `strace -f` file is counted line by line, independently of the profiler's
parser, and compared with the profile's per-syscall counts. Needs strace; run it in the container.
"""

import collections
import importlib.util
import pathlib
import re
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("profile_tool", ROOT / "scripts/syscall-profile.py")
tool = importlib.util.module_from_spec(spec)
spec.loader.exec_module(tool)

COMMANDS = (
    ["/bin/true"],
    ["python3", "-c", "pass"],
    ["sh", "-c", "ls / | wc -l"],
    ["python3", "-c",
     "import threading,socket\n"
     "t=threading.Thread(target=lambda: None); t.start(); t.join()\n"
     "socket.socket(socket.AF_INET, socket.SOCK_DGRAM).close()"],
)


def raw_counts(lines):
    """Count syscalls per name, counting a call once at its start or its resumption."""
    counts = collections.Counter()
    for line in lines:
        body = line.split(None, 1)[1] if len(line.split(None, 1)) > 1 else ""
        if body.startswith(("+++", "---")):
            continue
        if body.startswith("<... "):
            continue  # the unfinished half was counted
        match = re.match(r"(\w+)\(", body)
        if match:
            counts[tool.ALIASES.get(match[1], match[1])] += 1
    return counts


def check_multiprocess_verdicts():
    """The multi-process blocker of the vm loader: a blocker without --vm-multiprocess, a note with
    it, on x86_64 and on AArch64 alike; the direct loader never gets it. Needs no strace."""
    failures = 0
    for arch in ("x86_64", "aarch64"):
        profile = {"arch": arch, "calls": {}, "paths": {},
                   "multiprocess": {"tasks": 2, "processes": 2, "fork_like_calls": 1,
                                    "extra_execs": 1}}
        for loader, key, want in (("vm", False, "unlikely"), ("vm", True, "likely"),
                                  ("direct", False, "likely"), ("direct", True, "likely")):
            result = tool.check_loader(profile, loader, {}, {}, {}, 0.5, key)
            blocked = any(b["syscall"] == "fork/exec" for b in result["blockers"])
            noted = any(n["syscall"] == "fork/exec" for n in result["notes"])
            ok = result["verdict"] == want and blocked == (loader == "vm" and not key) and \
                noted == (loader == "vm" and key)
            failures += not ok
            print(f"{'ok' if ok else 'MISMATCH'}: multi-process verdict {arch} {loader} "
                  f"key={'on' if key else 'off'}: {result['verdict']}")
    return failures


def main():
    failures = check_multiprocess_verdicts()
    for command in COMMANDS:
        with tempfile.TemporaryDirectory() as tmp:
            trace = pathlib.Path(tmp) / "trace.txt"
            subprocess.run(["strace", "-f", "-s", "256", "-o", str(trace), "--", *command],
                           stdout=subprocess.DEVNULL, check=True)
            lines = trace.read_text(errors="replace").splitlines()
        raw = raw_counts(lines)
        raw["execve"] -= 1  # the profiler leaves out strace's own start of the command
        if not raw["execve"]:
            del raw["execve"]
        profile = tool.parse_trace(lines)
        mine = {name: data["count"] for name, data in profile["calls"].items()}
        diff = {name: (raw.get(name, 0), mine.get(name, 0))
                for name in set(raw) | set(mine) if raw.get(name, 0) != mine.get(name, 0)}
        multi = profile["multiprocess"]
        status = "ok" if not diff and not profile["unparsed_lines"] else "MISMATCH"
        failures += status != "ok"
        print(f"{status}: {' '.join(command)[:60]}: {sum(mine.values())} calls, "
              f"{len(mine)} syscalls, tasks {multi['tasks']}, processes {multi['processes']}, fork-like "
              f"{multi['fork_like_calls']}, extra execs {multi['extra_execs']}, "
              f"unparsed {profile['unparsed_lines']}")
        if diff:
            print("   raw vs profile:", dict(sorted(diff.items())))
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
