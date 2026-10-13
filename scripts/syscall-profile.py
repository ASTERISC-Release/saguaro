#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-or-later
"""Profile the syscalls of an application and check them against the syscall matrix.

  syscall-profile.py profile [-o FILE] -- CMD ARGS...   trace CMD with strace -f, write a JSON profile
  syscall-profile.py check FILE [--loader L] [--json]   verdict for direct and/or vm from a profile
  syscall-profile.py run -- CMD ARGS...                 profile, then check

The matrix is Documentation/devel/syscall-compat/syscalls.csv (made by scripts/syscall-matrix.py).
A dynamic profile covers only the code paths the traced run exercised. See
Documentation/devel/syscall-profile/PLAN.md.
"""

import argparse
import collections
import csv
import json
import os
import pathlib
import platform
import re
import shutil
import subprocess
import sys
import tempfile
import tomllib

ROOT = pathlib.Path(__file__).resolve().parents[1]
COMPAT = ROOT / "Documentation/devel/syscall-compat"
PROFILE_DIR = ROOT / "Documentation/devel/syscall-profile"

# strace name -> matrix name, where the matrix uses another spelling
ALIASES = {
    "clock_gettime64": "clock_gettime", "clock_nanosleep_time64": "clock_nanosleep",
    "futex_time64": "futex", "clock_settime64": "clock_settime", "clock_getres_time64": "clock_getres",
    "ppoll_time64": "ppoll", "pselect6_time64": "pselect6", "recvmmsg_time64": "recvmmsg",
    "sched_rr_get_interval_time64": "sched_rr_get_interval", "timerfd_gettime64": "timerfd_gettime",
    "timerfd_settime64": "timerfd_settime", "utimensat_time64": "utimensat",
    "stat64": "stat", "lstat64": "lstat", "fstat64": "fstat", "fstatat64": "newfstatat",
}

ARG_KEYS = {  # syscall -> (argument indexes that make the key)
    "socket": (0, 1), "ioctl": (1,), "prctl": (0,), "fcntl": (1,), "futex": (1,),
    "setsockopt": (1, 2), "getsockopt": (1, 2), "arch_prctl": (0,), "madvise": (2,),
    "rt_sigprocmask": (0,), "flock": (1,), "mremap": (3,), "sched_setscheduler": (1,),
}
PATH_CALLS = {"open": 0, "openat": 1, "openat2": 1, "stat": 0, "lstat": 0, "newfstatat": 1,
              "statx": 1, "access": 0, "faccessat": 1, "faccessat2": 1, "readlink": 0,
              "readlinkat": 1, "execve": 0, "execveat": 1, "chdir": 0, "getdents64": None}
SPECIAL_PATHS = ("/proc/", "/sys/", "/dev/")
MAX_ARG_KEYS = 40
MAX_PATHS = 300

LINE = re.compile(r"^(?P<pid>\d+)\s+(?P<body>.*)$")
RESULT = re.compile(r"\)\s+=\s+(?P<ret>-?\d+|0x[0-9a-f]+|\?)(?:\s+(?P<errno>E[A-Z0-9]+)\s+\(.*\))?"
                    r"(?:\s+\([^)]*\))?\s*$")


def split_args(text):
    """Split a strace argument string at top-level commas."""
    args, depth, quote, current, i = [], 0, False, [], 0
    while i < len(text):
        ch = text[i]
        if quote:
            current.append(ch)
            if ch == "\\":
                current.append(text[i + 1:i + 2])
                i += 1
            elif ch == '"':
                quote = False
        elif ch == '"':
            quote = True
            current.append(ch)
        elif ch in "{[(":
            depth += 1
            current.append(ch)
        elif ch in "}])":
            depth -= 1
            current.append(ch)
        elif ch == "," and depth == 0:
            args.append("".join(current).strip())
            current = []
        else:
            current.append(ch)
        i += 1
    if current:
        args.append("".join(current).strip())
    return args


def arg_key(name, args):
    """A short description of the arguments that matter for compatibility, or None."""
    if name in ("clone", "clone3"):
        match = re.search(r"flags=([A-Z_|0-9x]+)", ",".join(args))
        if match:
            flags = sorted(f for f in match.group(1).split("|") if f)
            return "|".join(flags)
        return None
    if name in ARG_KEYS:
        picked = [args[i] for i in ARG_KEYS[name] if i < len(args)]
        return ",".join(re.sub(r"\|?SOCK_(CLOEXEC|NONBLOCK)", "", p) for p in picked) or None
    return None


def parse_trace(lines):
    """Parse an `strace -f -o` file into a profile dictionary."""
    pending = {}  # pid -> (name, args text) of an unfinished call
    calls = collections.defaultdict(lambda: {"count": 0, "errors": collections.Counter(),
                                             "args": collections.Counter()})
    paths = collections.Counter()
    pids = set()
    unparsed = 0
    for raw in lines:
        match = LINE.match(raw.rstrip("\n"))
        if not match:
            continue
        pid, body = match["pid"], match["body"]
        pids.add(pid)
        if body.startswith(("+++", "---")):
            continue
        if body.startswith("<... "):  # resumed
            resumed = re.match(r"<\.\.\. (\w+) resumed>(.*)$", body)
            if not resumed or pid not in pending:
                unparsed += 1
                continue
            name, head = pending.pop(pid)
            body = f"{name}({head}{resumed[2]}"
        elif "<unfinished ...>" in body:
            started = re.match(r"(\w+)\((.*?)\s*<unfinished \.\.\.>$", body)
            if started:
                pending[pid] = (started[1], started[2])
            continue
        head = re.match(r"(\w+)\((.*)$", body)
        result = RESULT.search(body)
        if not head or not result:
            unparsed += 1
            continue
        name = ALIASES.get(head[1], head[1])
        arg_text = body[len(head[1]) + 1:result.start()]
        args = split_args(arg_text)
        entry = calls[name]
        entry["count"] += 1
        if result["errno"]:
            entry["errors"][result["errno"]] += 1
        key = arg_key(head[1], args)
        if key and (key in entry["args"] or len(entry["args"]) < MAX_ARG_KEYS):
            entry["args"][key] += 1
        if head[1] in PATH_CALLS and PATH_CALLS[head[1]] is not None and len(args) > PATH_CALLS[head[1]]:
            literal = re.match(r'"((?:[^"\\]|\\.)*)"', args[PATH_CALLS[head[1]]])
            if literal and literal[1].startswith(SPECIAL_PATHS) and (
                    literal[1] in paths or len(paths) < MAX_PATHS):
                paths[literal[1]] += 1
    def counted(name):
        return calls[name]["count"] if name in calls else 0

    process_count = len(pids)
    execs = counted("execve") + counted("execveat")
    thread_clone = sum(count for call in ("clone", "clone3") if call in calls
                       for key, count in calls[call]["args"].items() if "CLONE_THREAD" in key)
    process_clones = counted("clone") + counted("clone3") - thread_clone
    fork_like = counted("fork") + counted("vfork") + process_clones
    return {
        "calls": {name: {"count": data["count"], "errors": dict(data["errors"]),
                         "args": dict(data["args"])} for name, data in sorted(calls.items())},
        "paths": dict(sorted(paths.items())),
        "multiprocess": {"tasks": process_count, "processes": 1 + fork_like, "fork_like_calls": fork_like,
                         "extra_execs": max(execs - 1, 0)},
        "unparsed_lines": unparsed,
    }


def do_profile(command, output=None):
    strace = shutil.which("strace")
    if not strace:
        sys.exit("strace not found (in the container: apt-get install -y strace)")
    with tempfile.TemporaryDirectory() as tmp:
        trace = pathlib.Path(tmp) / "trace.txt"
        run = subprocess.run([strace, "-f", "-s", "256", "-o", str(trace), "--", *command])
        version = subprocess.run([strace, "-V"], capture_output=True, text=True).stdout.split("\n")[0]
        profile = parse_trace(trace.read_text(errors="replace").splitlines())
    profile.update({"version": 1, "command": command, "arch": platform.machine(),
                    "tracer": version, "exit_status": run.returncode})
    if output:
        pathlib.Path(output).write_text(json.dumps(profile, indent=1) + "\n")
    return profile


def load_matrix():
    with (COMPAT / "syscalls.csv").open() as f:
        syscalls = {row["syscall"]: row for row in csv.DictReader(f)}
    evidence = {}
    with (COMPAT / "matrix.csv").open() as f:
        for row in csv.DictReader(f):
            evidence[row["test"]] = row
    return syscalls, evidence


def load_rules():
    path = PROFILE_DIR / "rules.toml"
    return tomllib.loads(path.read_text()) if path.exists() else {}


PROCESS_WORDS = re.compile(r"fork|vfork|exec|clone|process", re.I)
FORK_LIKE = {"fork", "vfork", "clone", "clone3", "execve", "execveat", "wait4", "waitid"}
LOADER_COLUMN = {"direct": "direct", "vm": "vm_nofork"}


def usable_tests(name, loader, evidence):
    """LTP tests of one syscall that say something about it under `loader`.

    Left out: tests that also fail natively in the container (no evidence), and, for the vm, tests
    that fail only because the LTP harness or the test needs fork or exec (a design limit that the
    multi-process check reports separately), unless the syscall is itself fork-like.
    """
    rows = [row for row in evidence.values() if name in row["syscalls"].split(";")]
    usable = []
    for row in rows:
        if row["native"] != "pass":
            continue
        if (loader == "vm" and row["vm_nofork"] != "pass" and name not in FORK_LIKE
                and row["reason"] == "by design" and PROCESS_WORDS.search(row["evidence"])):
            continue
        usable.append(row)
    return rows, usable


def check_loader(profile, loader, matrix, evidence, rules, partial_ratio):
    arch = "aarch64" if profile["arch"] in ("aarch64", "arm64") else "x86_64"
    tolerated = {entry["syscall"]: entry for entry in rules.get("tolerated", [])}
    findings = {"blockers": [], "doubtful": [], "minor": [], "notes": [], "unknown": []}
    column = LOADER_COLUMN[loader]
    for name, data in profile["calls"].items():
        count = data["count"]
        row = matrix.get(name)
        if row is None:
            findings["unknown"].append({"syscall": name, "calls": count})
            continue
        state = row[f"{arch}_state"]
        if state == "ENOSYS":
            entry = {"syscall": name, "calls": count, "why": "ENOSYS in the LibOS"}
            if data["errors"].get("ENOSYS", 0) == count:
                entry["why"] += "; the application got ENOSYS from the host kernel too, so it copes"
                findings["notes"].append(entry)
            elif name in tolerated:
                entry["why"] += f"; tolerated: {tolerated[name]['why']} [{tolerated[name]['source']}]"
                findings["notes"].append(entry)
            else:
                findings["blockers"].append(entry)
            continue
        if state == "stub":
            findings["doubtful"].append({"syscall": name, "calls": count, "why": "a stub in the LibOS"})
            continue
        if arch != "x86_64":
            continue  # no LTP evidence for AArch64
        rows, usable = usable_tests(name, loader, evidence)
        if not rows:
            findings["notes"].append({"syscall": name, "calls": count,
                                      "why": "implemented, no LTP test"})
            continue
        if not usable:
            findings["notes"].append({"syscall": name, "calls": count,
                                      "why": f"implemented; {len(rows)} LTP test(s), none usable "
                                             "(they fail natively or only need fork)"})
            continue
        failing = [row for row in usable if row[column] != "pass"]
        if not failing:
            continue
        reasons = collections.Counter(row["reason"] for row in failing if row["reason"])
        entry = {"syscall": name, "calls": count,
                 "why": f"LTP {len(usable) - len(failing)} of {len(usable)} usable tests pass",
                 "reason": reasons.most_common(1)[0][0] if reasons else "",
                 "tests": [f"{row['test']}: {row['evidence'][:110]}" for row in failing[:2]]}
        if (len(usable) - len(failing)) / len(usable) >= partial_ratio:
            findings["minor"].append(entry)
        else:
            findings["doubtful"].append(entry)
    multi = profile["multiprocess"]
    if loader == "vm" and (multi["fork_like_calls"] or multi["extra_execs"]):
        findings["blockers"].append({"syscall": "fork/exec", "calls": multi["fork_like_calls"],
                                     "why": f"the application is multi-process ({multi['processes']} "
                                            f"processes, {multi['extra_execs']} extra execs); the VM PAL "
                                            "has one process by design"})
    for rule in rules.get("rule", []):
        if loader not in rule.get("loaders", ["direct", "vm"]):
            continue
        hit = rule_hit(rule, profile)
        if hit:
            bucket = "blockers" if rule["severity"] == "blocker" else "doubtful"
            findings[bucket].append({"syscall": rule.get("syscall", "path"), "calls": hit,
                                     "why": rule["why"] + f" [{rule['source']}]"})
    if findings["blockers"]:
        verdict = "unlikely"
    elif findings["doubtful"] or findings["unknown"]:
        verdict = "may run"
    else:
        verdict = "likely"
    return {"loader": loader, "verdict": verdict, "arch": arch, **findings}


def rule_hit(rule, profile):
    """Number of matches of one rule table entry in the profile."""
    total = 0
    if "path_prefix" in rule:
        total += sum(n for p, n in profile["paths"].items() if p.startswith(rule["path_prefix"]))
    if "syscall" in rule and "arg_contains" in rule:
        total += sum(n for key, n in profile["calls"].get(rule["syscall"], {}).get("args", {}).items()
                     if rule["arg_contains"] in key)
    return total


def report_text(profile, results):
    multi = profile["multiprocess"]
    lines = [f"command: {' '.join(profile['command'])}",
             f"{sum(c['count'] for c in profile['calls'].values())} calls of {len(profile['calls'])} "
             f"syscalls, {multi['processes']} process(es), exit status {profile['exit_status']}, "
             f"arch {profile['arch']}",
             "Only the code paths of this run are covered; the matrix was measured on x86_64."]
    if profile["arch"] not in ("x86_64", "amd64"):
        lines.append("Not x86_64: only the LibOS source state is used (no LTP evidence).")
    for res in results:
        lines += ["", f"== {res['loader']}: {res['verdict'].upper()}"]
        for title, key in (("blocking", "blockers"), ("doubtful", "doubtful"),
                           ("minor LTP failures", "minor"), ("not in the matrix", "unknown"),
                           ("notes", "notes")):
            items = sorted(res[key], key=lambda e: -e["calls"])
            if not items:
                continue
            shown = items if key in ("blockers", "doubtful", "unknown") else items[:8]
            lines.append(f"  {title}:")
            for item in shown:
                extra = f" ({item['reason']})" if item.get("reason") else ""
                lines.append(f"    {item['syscall']:<22} {item['calls']:>7} calls  "
                             f"{item.get('why', 'not in the matrix')}{extra}")
                for sample in item.get("tests", [])[:1 if key != "doubtful" else 2]:
                    lines.append(f"        {sample}")
            if len(items) > len(shown):
                lines.append(f"    ... {len(items) - len(shown)} more")
    return "\n".join(lines)


def do_check(profile, loaders, as_json, partial_ratio):
    matrix, evidence = load_matrix()
    rules = load_rules()
    results = [check_loader(profile, loader, matrix, evidence, rules, partial_ratio)
               for loader in loaders]
    if as_json:
        print(json.dumps({"command": profile["command"], "results": results}, indent=1))
    else:
        print(report_text(profile, results))
    return results


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="mode", required=True)
    for mode in ("profile", "run"):
        p = sub.add_parser(mode)
        p.add_argument("-o", "--output", help="write the JSON profile here")
        p.add_argument("command", nargs=argparse.REMAINDER, help="-- CMD ARGS...")
    for mode in ("check", "run"):
        p = sub.choices[mode] if mode == "run" else sub.add_parser(mode)
        if mode == "check":
            p.add_argument("profile", type=pathlib.Path)
        p.add_argument("--loader", choices=("direct", "vm", "both"), default="both")
        p.add_argument("--json", action="store_true")
        p.add_argument("--partial-ratio", type=float, default=0.5,
                       help="LTP pass ratio from which a partial syscall is only a minor note")
    args = parser.parse_args()
    loaders = ("direct", "vm") if getattr(args, "loader", "both") == "both" else (args.loader,)
    if args.mode == "check":
        do_check(json.loads(args.profile.read_text()), loaders, args.json, args.partial_ratio)
        return
    command = args.command[1:] if args.command[:1] == ["--"] else args.command
    if not command:
        parser.error("no command given after --")
    profile = do_profile(command, args.output)
    if args.mode == "profile":
        print(f"{len(profile['calls'])} syscalls, {profile['multiprocess']['processes']} process(es)"
              + (f"; wrote {args.output}" if args.output else ""), file=sys.stderr)
        if not args.output:
            print(json.dumps(profile, indent=1))
    else:
        do_check(profile, loaders, args.json, args.partial_ratio)


if __name__ == "__main__":
    main()
