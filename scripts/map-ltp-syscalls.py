#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-or-later
"""Map LTP runtest/syscalls test names to Linux syscall names."""

import argparse
import csv
import json
import os
import pathlib
import re
import shlex

ROOT = pathlib.Path(__file__).resolve().parents[1]
# The syscall-compat and syscall-profile data live in the private development-documents repository, checked out
# next to this one (SAGUARO_DEV_DOCS names another place).
DEV_DOCS = pathlib.Path(os.environ.get("SAGUARO_DEV_DOCS", ROOT.parent / "gramine-multiarch-docs"))
OVERRIDES = {
    "execl": ["execve"], "execle": ["execve"], "execlp": ["execve"],
    "execv": ["execve"], "execvp": ["execve"], "wait": ["wait4"],
    "waitpid": ["wait4"], "statvfs": ["statfs"], "fstatvfs": ["fstatfs"],
    "fstatat": ["newfstatat"], "fstatat64": ["newfstatat"],
    "sigaction": ["rt_sigaction"], "sigprocmask": ["rt_sigprocmask"],
    "sigpending": ["rt_sigpending"], "sigsuspend": ["rt_sigsuspend"],
    "sigtimedwait": ["rt_sigtimedwait"], "sigwaitinfo": ["rt_sigtimedwait"],
    "futex_wait": ["futex"], "futex_wake": ["futex"],
    "futex_cmp_requeue": ["futex"], "futex_wait_bitset": ["futex"],
    "bpf_map": ["bpf"], "bpf_prog": ["bpf"], "epoll": ["epoll_create1", "epoll_ctl", "epoll_wait"],
    "eventfd2": ["eventfd2"], "dup3": ["dup3"], "pipe2": ["pipe2"],
    "inotify_init1": ["inotify_init1"], "inotify": ["inotify_add_watch", "inotify_rm_watch"],
    "fanotify": ["fanotify_init", "fanotify_mark"], "ioctl_loop": ["ioctl"],
    "ioctl_ns": ["ioctl"], "ioctl_sg": ["ioctl"], "sockioctl": ["ioctl"],
    "pread": ["pread64"], "pwrite": ["pwrite64"], "newuname": ["uname"],
    "gethostname": ["uname"], "getdomainname": ["uname"], "getdtablesize": ["getrlimit"],
    "gethostbyname_r": ["socket"], "sbrk": ["brk"], "llseek": ["lseek"],
    "posix_fadvise": ["fadvise64"], "stat": ["newfstatat"], "time": ["time"],
    "abort": ["tgkill"], "signal": ["rt_sigaction"], "recv": ["recvfrom", "recvmsg"],
    "send": ["sendto", "sendmsg"], "pselect": ["pselect6"],
    "timerfd": ["timerfd_create", "timerfd_settime", "timerfd_gettime"],
    "futex": ["futex"], "inotify_init": ["inotify_init"],
}


def match_suffix(test_binary, name):
    if not test_binary.startswith(name):
        return False
    suffix = test_binary[len(name):]
    return not suffix or bool(re.fullmatch(r"[_-]?\d+(?:[_-]\d+)*(?:_(?:16|32|64))?", suffix))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--scenario", type=pathlib.Path, default=ROOT / ".ltp-src/runtest/syscalls")
    parser.add_argument("--inventory", type=pathlib.Path, default=DEV_DOCS / "syscall-compat/inventory.csv")
    parser.add_argument("--output", type=pathlib.Path, default=DEV_DOCS / "syscall-compat/mapping.toml")
    args = parser.parse_args()
    syscall_names = {row["syscall"] for row in csv.DictReader(args.inventory.open())}
    candidates = sorted({n for n in syscall_names if n != "misc"} | set(OVERRIDES), key=len, reverse=True)
    mapped, unmapped = [], []
    for line in args.scenario.read_text().splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        fields = shlex.split(line)
        test_name, command = fields[0], fields[1:]
        binary = pathlib.Path(command[0]).name if command else test_name
        matched = next((name for name in candidates if match_suffix(test_name, name)), None)
        if not matched:
            matched = next((name for name in candidates if match_suffix(binary, name)), None)
        calls = OVERRIDES.get(matched) if matched else None
        if calls is None and matched:
            call = matched
            if call not in syscall_names:
                calls = []
            else:
                calls = [call]
        if calls is None:
            call = None
            calls = [call] if call else []
        item = (test_name, calls)
        (mapped if calls else unmapped).append(item)

    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("w") as output:
        output.write("# Generated from LTP runtest/syscalls; heuristic mappings and explicit unmapped names.\n")
        for name, calls in mapped:
            output.write("[[test]]\nname = {}\nsyscalls = {}\n\n".format(
                json.dumps(name), json.dumps(calls)))
        output.write("[unmapped]\ntests = {}\n".format(json.dumps([name for name, _ in unmapped])))
    print(f"tests={len(mapped) + len(unmapped)} mapped={len(mapped)} unmapped={len(unmapped)}")
    counts = {}
    for _, calls in mapped:
        for call in calls:
            counts[call] = counts.get(call, 0) + 1
    for name, count in sorted(counts.items()):
        print(f"{name}: {count}")
    if unmapped:
        print("unmapped: " + ", ".join(name for name, _ in unmapped))


if __name__ == "__main__":
    main()
