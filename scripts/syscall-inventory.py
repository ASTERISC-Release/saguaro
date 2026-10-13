#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-or-later
"""Inventory Linux syscall numbers and Gramine LibOS table coverage."""

import argparse
import collections
import pathlib
import re

ROOT = pathlib.Path(__file__).resolve().parents[1]
GROUPS = {
    "file": "read write open close stat fstat lstat newfstatat statx access faccessat faccessat2 lseek readlink readlinkat getdents getdents64 chdir fchdir getcwd mkdir mkdirat rmdir rename renameat renameat2 link linkat unlink unlinkat symlink symlinkat chmod fchmod fchmodat chown fchown fchownat lchown umask mknod mknodat truncate ftruncate fallocate fsync fdatasync sync syncfs flock fcntl ioctl dup dup2 dup3 pipe pipe2 sendfile copy_file_range splice tee vmsplice".split(),
    "memory": "brk mmap munmap mremap mprotect madvise mincore msync mlock munlock mlockall munlockall remap_file_pages pkey_mprotect pkey_alloc pkey_free process_madvise memfd_create memfd_secret".split(),
    "process": "clone clone3 fork vfork execve execveat exit exit_group wait4 waitid getpid getppid gettid getuid geteuid getgid getegid setuid setgid setreuid setregid setresuid setresgid getresuid getresgid getgroups setgroups setfsuid setfsgid getpgid setpgid getpgrp setsid getsid prctl arch_prctl personality ptrace capget capset pidfd_open pidfd_getfd pidfd_send_signal process_mrelease".split(),
    "signal": "kill tkill tgkill rt_sigaction rt_sigprocmask rt_sigpending rt_sigtimedwait rt_sigqueueinfo rt_sigsuspend rt_sigreturn sigaltstack pause signalfd signalfd4".split(),
    "socket": "socket socketpair bind listen accept accept4 connect shutdown getsockname getpeername sendto recvfrom sendmsg recvmsg recvmmsg sendmmsg setsockopt getsockopt socketcall".split(),
    "ipc": "semget semop semctl semtimedop shmget shmat shmdt shmctl msgget msgsnd msgrcv msgctl", 
    "time": "time gettimeofday settimeofday clock_gettime clock_gettime64 clock_settime clock_settime64 clock_getres clock_nanosleep nanosleep timer_create timer_settime timer_gettime timer_getoverrun timer_delete alarm getitimer setitimer times adjtimex clock_adjtime utimes utimensat futex futex_waitv".split(),
}
GROUPS["ipc"] = GROUPS["ipc"].split()
GROUP_BY_NAME = {name: group for group, names in GROUPS.items() for name in names}


def numbers(path, generic=False):
    result = {}
    for line in path.read_text().splitlines():
        match = re.match(r"\s*#define\s+__(?:NR|NR3264)_([A-Za-z0-9_]+)\s+(\d+)\b", line)
        if match:
            name, number = match.group(1), int(match.group(2))
            result.setdefault(name, number)
    return result


def table_entries(path):
    return {name: target.strip() for name, target in re.findall(
        r"\[__NR_([A-Za-z0-9_]+)\]\s*=\s*\(libos_syscall_t\)([^,]+)", path.read_text())}


def function_bodies():
    bodies = {}
    declaration = re.compile(r"\blong\s+(libos_syscall_[A-Za-z0-9_]+)\s*\([^;]*?\)\s*\{", re.S)
    for path in (ROOT / "libos/src").rglob("*.c"):
        source = re.sub(r"/\*.*?\*/|//[^\n]*", "", path.read_text(), flags=re.S)
        for match in declaration.finditer(source):
            start = match.end()
            depth = 1
            end = start
            while end < len(source) and depth:
                depth += (source[end] == "{") - (source[end] == "}")
                end += 1
            bodies[match.group(1)] = source[start:end - 1].strip()
    return bodies


def classify(target, bodies):
    if not target or target == "0":
        return "ENOSYS"
    handler = target.removeprefix("libos_syscall_")
    body = bodies.get("libos_syscall_" + handler, "")
    # The conservative stub rule: the complete handler body is one fixed-value return.
    # More involved handlers are implemented; this intentionally does not infer quality.
    if not body or re.fullmatch(r"return\s+(?:[-+]?\d+|[-+]?ENOSYS|0)\s*;", body):
        return "stub"
    return "implemented"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--x86-header", type=pathlib.Path, default=pathlib.Path("/usr/include/x86_64-linux-gnu/asm/unistd_64.h"))
    parser.add_argument("--arm64-header", type=pathlib.Path, default=pathlib.Path("/usr/include/asm-generic/unistd.h"))
    parser.add_argument("--output", type=pathlib.Path, default=ROOT / "Documentation/devel/syscall-compat/inventory.csv")
    args = parser.parse_args()
    arch_data = {
        "x86_64": (numbers(args.x86_header), table_entries(ROOT / "libos/src/arch/x86_64/libos_table.c")),
        "aarch64": (numbers(args.arm64_header, generic=True), table_entries(ROOT / "libos/src/arch/aarch64/libos_table.c")),
    }
    bodies = function_bodies()
    syscalls = collections.defaultdict(dict)
    for arch, (nrmap, entries) in arch_data.items():
        for name, number in nrmap.items():
            # The arm64 generic header exposes several 32/64-bit aliases; retain the canonical name.
            syscalls[name][arch] = number
            if name not in GROUP_BY_NAME:
                GROUP_BY_NAME[name] = "misc"
        for name, target in entries.items():
            syscalls[name][f"{arch}_state"] = classify(target, bodies)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("w") as out:
        out.write("syscall,x86_64_number,aarch64_number,group,x86_64_state,aarch64_state\n")
        for name in sorted(syscalls, key=lambda key: min(
                (v for k, v in syscalls[key].items() if k in ("x86_64", "aarch64")),
                default=10000)):
            row = syscalls[name]
            group = GROUP_BY_NAME.get(name, "misc")
            out.write(f"{name},{row.get('x86_64','')},{row.get('aarch64','')},{group},{row.get('x86_64_state','ENOSYS')},{row.get('aarch64_state','ENOSYS')}\n")
    print(f"wrote {len(syscalls)} syscall names to {args.output}")
    for group in ["file", "memory", "process", "signal", "socket", "ipc", "time", "misc"]:
        selected = [(n, v) for n, v in syscalls.items() if GROUP_BY_NAME.get(n, "misc") == group]
        states = {
            arch: dict(collections.Counter(v.get(f"{arch}_state", "ENOSYS") for _, v in selected
                                           if arch in v))
            for arch in ("x86_64", "aarch64")
        }
        print(f"{group}: {len(selected)} names (x86_64 {states['x86_64']}; aarch64 {states['aarch64']})")


if __name__ == "__main__":
    main()
