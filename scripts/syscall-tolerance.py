#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-or-later
"""Does an application tolerate the loss of the syscalls the LibOS does not support?

Takes the syscalls that scripts/syscall-profile.py reports as ENOSYS in the LibOS (or a list you
give), runs the program natively with exactly those syscalls answered by -ENOSYS (a seccomp-bpf
filter installed before exec, so raw syscalls and every thread and child are covered, which an
LD_PRELOAD shim would not do), and compares the result with an undisturbed run.

  syscall-tolerance.py --loader direct -- CMD ARGS...        profile, deny, compare
  syscall-tolerance.py --deny utimensat,statx -- CMD ARGS... deny the given syscalls
  syscall-tolerance.py --profile app.json --loader vm -- CMD  reuse a saved profile
  syscall-tolerance.py --drive "curl -s localhost:8080/" -- nginx ...   a server and its client

If the denied run differs, each syscall is denied alone to find the one that matters.
Needs strace for the profiling step (see syscall-profile.py). Linux, x86_64 or aarch64.
"""

import argparse
import ctypes
import importlib.util
import json
import os
import pathlib
import platform
import signal
import subprocess
import sys
import time

ROOT = pathlib.Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("profile_tool", ROOT / "scripts/syscall-profile.py")
tool = importlib.util.module_from_spec(spec)
spec.loader.exec_module(tool)

ARCH = {"x86_64": ("x86_64", 0xC000003E), "aarch64": ("aarch64", 0xC00000B7)}
ENOSYS = 38
PR_SET_NO_NEW_PRIVS, PR_SET_SECCOMP, SECCOMP_MODE_FILTER = 38, 22, 2


class SockFilter(ctypes.Structure):
    _fields_ = [("code", ctypes.c_uint16), ("jt", ctypes.c_uint8), ("jf", ctypes.c_uint8),
                ("k", ctypes.c_uint32)]


class SockFprog(ctypes.Structure):
    _fields_ = [("len", ctypes.c_uint16), ("filter", ctypes.POINTER(SockFilter))]


def syscall_numbers():
    machine = platform.machine()
    if machine not in ARCH:
        sys.exit(f"unsupported architecture {machine}")
    column = f"{ARCH[machine][0]}_number"
    with (tool.COMPAT / "syscalls.csv").open() as f:
        import csv
        return {r["syscall"]: int(r[column]) for r in csv.DictReader(f) if r[column]}


def install_filter(names):
    """Make the listed syscalls return -ENOSYS for this process and everything it starts."""
    numbers = syscall_numbers()
    unknown = [n for n in names if n not in numbers]
    if unknown:
        sys.exit("no syscall number on this architecture for: " + ", ".join(unknown))
    audit_arch = ARCH[platform.machine()][1]
    count = len(names)
    allow, deny = 3 + count, 4 + count
    prog = [SockFilter(0x20, 0, 0, 4),                                  # ld arch
            SockFilter(0x15, 0, allow - 2, audit_arch),                 # other ABI: allow
            SockFilter(0x20, 0, 0, 0)]                                  # ld syscall number
    for index, name in enumerate(names):
        prog.append(SockFilter(0x15, deny - (3 + index + 1), 0, numbers[name]))
    prog += [SockFilter(0x06, 0, 0, 0x7fff0000),                        # allow
             SockFilter(0x06, 0, 0, 0x00050000 | ENOSYS)]               # errno ENOSYS
    array = (SockFilter * len(prog))(*prog)
    fprog = SockFprog(len(prog), array)
    libc = ctypes.CDLL(None, use_errno=True)
    if libc.prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0:
        sys.exit("prctl(NO_NEW_PRIVS) failed")
    if libc.prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, ctypes.byref(fprog)) != 0:
        sys.exit(f"installing the seccomp filter failed (errno {ctypes.get_errno()})")


def run_once(command, deny, drive, wait, timeout, capture):
    """One run; returns (exit status or 'hang', drive status, stdout)."""
    argv = [sys.executable, str(pathlib.Path(__file__).resolve()), "--exec-denied",
            ",".join(deny), "--", *command]
    proc = subprocess.Popen(argv, stdout=subprocess.PIPE if capture else subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL, start_new_session=True)
    drive_status = None
    deadline = time.time() + timeout
    try:
        if drive:
            time.sleep(wait)
            drive_status = subprocess.run(drive, shell=True, stdout=subprocess.DEVNULL,
                                          stderr=subprocess.DEVNULL,
                                          timeout=max(deadline - time.time(), 1)).returncode
            if proc.poll() is not None:  # it ended before the client finished
                out = proc.communicate()[0]
                return proc.returncode, drive_status, out
            os.killpg(proc.pid, signal.SIGTERM)
            out, _ = proc.communicate(timeout=max(deadline - time.time(), 1))
            return "stopped", drive_status, out
        out, _ = proc.communicate(timeout=max(deadline - time.time(), 1))
        return proc.returncode, drive_status, out
    except subprocess.TimeoutExpired:
        os.killpg(proc.pid, signal.SIGKILL)
        proc.communicate()
        return "hang", drive_status, None


def same(base, other, compare_output, drive):
    """Equal outcomes: exit status ("stopped" for a program still running when we ended it, so an
    early exit or crash differs) and drive status."""
    if base[1] != other[1] or base[0] != other[0]:
        return False
    return not compare_output or base[2] == other[2]


def describe(result):
    status = result[0] if result[0] in ("hang", "stopped") else f"exit {result[0]}"
    return status + (f", client {result[1]}" if result[1] is not None else "")


def unsupported_from_profile(profile, loader):
    matrix, evidence = tool.load_matrix()
    res = tool.check_loader(profile, loader, matrix, evidence, tool.load_rules(), 0.5)
    return sorted({e["syscall"] for e in res["blockers"] + res["notes"]
                   if e["why"].startswith("ENOSYS in the LibOS")}), res


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--loader", choices=("direct", "vm"), default="direct",
                        help="whose ENOSYS list to use (default direct)")
    parser.add_argument("--deny", help="comma-separated syscalls to deny instead of the report's")
    parser.add_argument("--profile", type=pathlib.Path, help="a saved profile instead of tracing")
    parser.add_argument("--drive", help="server mode: client command run after --wait seconds")
    parser.add_argument("--wait", type=float, default=2.0)
    parser.add_argument("--timeout", type=float, default=60.0, help="seconds per run")
    parser.add_argument("--compare-output", action="store_true", help="also compare stdout")
    parser.add_argument("--exec-denied", metavar="LIST", help=argparse.SUPPRESS)
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    command = args.command[1:] if args.command[:1] == ["--"] else args.command
    if not command:
        parser.error("no command given after --")

    if args.exec_denied is not None:  # internal: install the filter, then become the program
        install_filter([n for n in args.exec_denied.split(",") if n])
        os.execvp(command[0], command)

    if args.deny:
        deny = sorted(set(args.deny.split(",")))
        print(f"denying (given): {', '.join(deny)}")
    else:
        profile = (json.loads(args.profile.read_text()) if args.profile else
                   tool.do_profile(command, None, args.drive, args.wait))
        deny, _ = unsupported_from_profile(profile, args.loader)
        print(f"syscalls the {args.loader} loader reports as ENOSYS and the program calls: "
              f"{', '.join(deny) or 'none'}")
        if not deny:
            print("nothing to deny; the program does not call any such syscall in this run")
            return

    capture = args.compare_output
    base = run_once(command, [], args.drive, args.wait, args.timeout, capture)
    print(f"undisturbed:        {describe(base)}")
    if base[0] == "hang" or (not args.drive and base[0] != 0) or (args.drive and base[0] != "stopped"):
        print("the undisturbed run did not succeed; fix the command first")
        sys.exit(2)
    denied = run_once(command, deny, args.drive, args.wait, args.timeout, capture)
    ok = same(base, denied, args.compare_output, args.drive)
    print(f"all denied:         {describe(denied)}   -> {'TOLERATED' if ok else 'DIFFERS'}")
    if ok:
        print("the program behaves the same without these syscalls (for this run)")
        return
    print("denying each syscall alone:")
    culprits = []
    for name in deny:
        alone = run_once(command, [name], args.drive, args.wait, args.timeout, capture)
        fine = same(base, alone, args.compare_output, args.drive)
        print(f"  {name:<22} {describe(alone):<24} {'ok' if fine else 'BREAKS IT'}")
        if not fine:
            culprits.append(name)
    print("needed syscalls: " + (", ".join(culprits) if culprits else
                                 "none alone (the combination breaks it)"))
    sys.exit(1)


if __name__ == "__main__":
    main()
