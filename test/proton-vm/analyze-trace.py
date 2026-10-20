#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The ASTERISC Research Lab
"""Summarise a `strace -f -ttt -y` trace of a Wine/Proton run for the feasibility study
(dev docs: proton-vm/): processes and threads (clone flags, execve), syscall counts,
ioctls by request, fixed-address and large mmaps, signals delivered, prctl/arch_prctl/modify_ldt,
futex_waitv/eventfd/SCM_RIGHTS use, files opened under /proc, /sys and /dev, and the result of
the calls Wine relies on. Usage: analyze-trace.py TRACE.strace [--top N]"""

import argparse
import collections
import re
import sys

LINE = re.compile(r"^(\d+)\s+(\d+\.\d+)\s+(.*)$")
CALL = re.compile(r"^([a-z_0-9]+)\((.*)\)\s+=\s+(-?\w+)(.*)$")
UNFINISHED = re.compile(r"^([a-z_0-9]+)\((.*)<unfinished \.\.\.>$")
RESUMED = re.compile(r"^<\.\.\. ([a-z_0-9]+) resumed>(.*)\)\s+=\s+(-?\w+)(.*)$")
SIGNAL = re.compile(r"^--- (SIG\w+) \{(.*)\} ---$")
KILLED = re.compile(r"^\+\+\+ killed by (SIG\w+)")
EXITED = re.compile(r"^\+\+\+ exited with (\d+)")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("trace")
    ap.add_argument("--top", type=int, default=40)
    args = ap.parse_args()

    counts = collections.Counter()
    errors = collections.defaultdict(collections.Counter)
    ioctls = collections.Counter()
    ioctl_err = collections.Counter()
    clone_flags = collections.Counter()
    execs = []
    signals = collections.Counter()
    sig_by_pid = collections.Counter()
    killed = collections.Counter()
    prctls = collections.Counter()
    arch_prctls = collections.Counter()
    fixed_mmaps = []
    big_mmaps = []
    opens = collections.Counter()
    open_fail = collections.Counter()
    scm_rights = 0
    sockets = collections.Counter()
    sigactions = collections.Counter()
    first_ts = last_ts = None
    pids = set()
    unfinished = {}
    interesting = ("futex_waitv", "eventfd2", "memfd_create", "modify_ldt", "set_thread_area", "sigaltstack",
                   "membarrier", "rseq", "process_vm_readv", "process_vm_writev", "ptrace", "pidfd_open",
                   "sched_setaffinity", "sched_setscheduler", "setpriority", "timerfd_create", "signalfd4",
                   "epoll_create1", "inotify_init1", "personality", "mremap", "madvise", "msync", "mlock",
                   "shmget", "semget", "ioperm", "iopl", "userfaultfd", "io_uring_setup", "kcmp", "seccomp",
                   "unshare", "setns", "clone3", "vfork", "fork", "execveat", "statx", "faccessat2", "openat2",
                   "futex", "rt_sigprocmask", "rt_sigreturn", "poll", "ppoll", "recvmsg", "sendmsg", "writev",
                   "readlink", "readlinkat", "getdents64", "getrandom", "sysinfo", "uname", "prlimit64", "setrlimit")
    seen = collections.defaultdict(collections.Counter)   # syscall -> result counter for interesting ones
    for raw in open(args.trace, errors="replace"):
        m = LINE.match(raw.rstrip("\n"))
        if not m:
            continue
        pid, ts, rest = int(m.group(1)), float(m.group(2)), m.group(3)
        pids.add(pid)
        first_ts = ts if first_ts is None else first_ts
        last_ts = ts
        sm = SIGNAL.match(rest)
        if sm:
            signals[sm.group(1)] += 1
            sig_by_pid[(pid, sm.group(1))] += 1
            continue
        km = KILLED.match(rest)
        if km:
            killed[km.group(1)] += 1
            continue
        if EXITED.match(rest):
            continue
        um = UNFINISHED.match(rest)
        if um:
            unfinished[pid] = (um.group(1), um.group(2))
            continue
        rm = RESUMED.match(rest)
        if rm:
            name = rm.group(1)
            start = unfinished.pop(pid, (name, ""))[1]
            argstr = start + rm.group(2)
            result, tail = rm.group(3), rm.group(4)
        else:
            cm = CALL.match(rest)
            if not cm:
                continue
            name, argstr, result, tail = cm.groups()
        counts[name] += 1
        err = None
        if result == "-1":
            em = re.match(r"\s+(E[A-Z0-9]+)", tail)
            err = em.group(1) if em else "?"
            errors[name][err] += 1
        if name in interesting:
            seen[name][err or "ok"] += 1
        if name == "ioctl":
            parts = argstr.split(", ")
            req = parts[1] if len(parts) > 1 else "?"
            fd = parts[0]
            dev = re.search(r"<([^>]*)>", fd)
            key = f"{req} ({dev.group(1).split('/')[-1] if dev else '?'})"
            ioctls[key] += 1
            if err:
                ioctl_err[f"{key} -> {err}"] += 1
        elif name in ("clone", "clone3"):
            fm = re.search(r"flags=([A-Z_|0-9x]+)", argstr)
            clone_flags[fm.group(1) if fm else argstr[:60]] += 1
        elif name == "execve" and not err:
            execs.append(argstr.split(",")[0].strip('"'))
        elif name == "prctl":
            prctls[argstr.split(",")[0] + (" -> " + err if err else "")] += 1
        elif name == "arch_prctl":
            arch_prctls[argstr.split(",")[0] + (" -> " + err if err else "")] += 1
        elif name in ("mmap", "mprotect", "munmap"):
            parts = [p.strip() for p in argstr.split(",")]
            if name == "mmap" and len(parts) >= 4:
                addr, size, prot, flags = parts[0], parts[1], parts[2], parts[3]
                try:
                    sz = int(size, 0)
                except ValueError:
                    sz = 0
                if "MAP_FIXED" in flags and addr != "NULL":
                    fixed_mmaps.append((addr, sz, prot, flags, result))
                if sz >= 1 << 30:
                    big_mmaps.append((addr, sz, prot, flags, result))
        elif name in ("openat", "open", "readlink", "readlinkat", "access", "stat", "statx", "newfstatat"):
            pm = re.search(r'"([^"]*)"', argstr)
            if pm:
                path = pm.group(1)
                if path.startswith(("/proc", "/sys", "/dev")):
                    (open_fail if err else opens)[f"{name} {path}" + (f" -> {err}" if err else "")] += 1
        elif name in ("sendmsg", "recvmsg"):
            if "SCM_RIGHTS" in argstr:
                scm_rights += 1
        elif name == "socket":
            sockets[argstr] += 1
        elif name == "rt_sigaction":
            parts = argstr.split(", ")
            if len(parts) > 1 and "sa_handler" in parts[1] and "SIG_DFL" not in parts[1] and "SIG_IGN" not in parts[1]:
                flags = re.search(r"sa_flags=([A-Z_|]+)", argstr)
                sigactions[parts[0] + " " + (flags.group(1) if flags else "")] += 1

    total = sum(counts.values())
    print(f"trace: {args.trace}")
    print(f"duration {last_ts - first_ts:.2f} s, {len(pids)} tasks (processes+threads), {total} syscalls")
    print(f"\nexecve ({len(execs)}): " + ", ".join(execs))
    print("\nclone flags:")
    for k, v in clone_flags.most_common():
        print(f"  {v:6d}  {k}")
    print(f"\nsyscalls (top {args.top}):")
    for k, v in counts.most_common(args.top):
        e = errors[k]
        es = ", ".join(f"{n} {c}" for n, c in e.most_common(4))
        print(f"  {v:8d}  {k}" + (f"   [{es}]" if es else ""))
    print("\ncalls Wine relies on (result counts):")
    for k in interesting:
        if seen[k]:
            print(f"  {k}: " + ", ".join(f"{r} {c}" for r, c in seen[k].most_common()))
    print("\nioctls:")
    for k, v in ioctls.most_common(60):
        print(f"  {v:6d}  {k}")
    if ioctl_err:
        print("ioctl errors:")
        for k, v in ioctl_err.most_common(20):
            print(f"  {v:6d}  {k}")
    print("\nprctl:")
    for k, v in prctls.most_common():
        print(f"  {v:6d}  {k}")
    print("arch_prctl:")
    for k, v in arch_prctls.most_common():
        print(f"  {v:6d}  {k}")
    print(f"\nMAP_FIXED* mmaps ({len(fixed_mmaps)}; first 40 distinct by address/size):")
    seen_fixed = set()
    n = 0
    for addr, sz, prot, flags, result in fixed_mmaps:
        key = (addr, sz)
        if key in seen_fixed:
            continue
        seen_fixed.add(key)
        n += 1
        if n > 40:
            break
        print(f"  {addr:>16} +{sz:#14x} {prot:<32} {flags:<60} = {result}")
    print(f"\nmmaps >= 1 GiB ({len(big_mmaps)}):")
    for addr, sz, prot, flags, result in big_mmaps[:20]:
        print(f"  {addr:>16} +{sz:#14x} {prot:<32} {flags:<60} = {result}")
    print("\nsignals delivered:")
    for k, v in signals.most_common():
        print(f"  {v:6d}  {k}")
    if killed:
        print("killed by: " + ", ".join(f"{k} {v}" for k, v in killed.items()))
    print("\nsignal handlers installed (signal, flags):")
    for k, v in sigactions.most_common():
        print(f"  {v:6d}  {k}")
    print(f"\nSCM_RIGHTS messages: {scm_rights}")
    print("sockets: " + "; ".join(f"{k} x{v}" for k, v in sockets.most_common(8)))
    print("\n/proc, /sys, /dev paths opened or stat'ed (successful):")
    for k, v in opens.most_common(60):
        print(f"  {v:6d}  {k}")
    print("failed:")
    for k, v in open_fail.most_common(40):
        print(f"  {v:6d}  {k}")


if __name__ == "__main__":
    main()
