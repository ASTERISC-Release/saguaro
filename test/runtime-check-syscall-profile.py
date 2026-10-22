#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-or-later
"""Ground truth for scripts/syscall-profile.py: run programs under saguaro-direct with the log
level `warning` and compare the syscalls the LibOS reports as unsupported ("Unsupported system
call X") with the syscalls the profile says are ENOSYS in the LibOS.

Run in the container with an install of the direct PAL (`. ~/.local/saguaro-vm/env.sh`); needs
strace, gcc and busybox-static. See dev docs: syscall-profile/TESTING.md.
"""

import importlib.util
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("profile_tool", ROOT / "scripts/syscall-profile.py")
tool = importlib.util.module_from_spec(spec)
spec.loader.exec_module(tool)

# raw syscalls the control program makes; each is ENOSYS in the x86_64 LibOS, except the last three
CONTROL_C = r'''
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <unistd.h>
#define T(name, ...) do { long r = syscall(SYS_##name, ##__VA_ARGS__); \
    printf("%-14s ret %ld errno %d\n", #name, r, r < 0 ? errno : 0); } while (0)
int main(void) {
    T(inotify_init1, 0);
    T(timerfd_create, 1, 0);
    T(utimensat, AT_FDCWD, "/nonexistent", 0, 0);
    T(symlinkat, "/a", AT_FDCWD, "/tmp/syscall-profile-link");
    T(setxattr, "/nonexistent", "user.a", "v", 1, 0);
    T(splice, -1, 0, -1, 0, 1, 0);
    T(mq_open, "/x", 0, 0, 0);
    T(rseq, 0, 0, 0, 0);
    T(statx, AT_FDCWD, "/", 0, 0xfff, 0);
    T(getpid);
    T(getuid);
    T(gettid);
    return 0;
}
'''
HELLO_C = '#include <stdio.h>\nint main(void){puts("Hello");return 0;}\n'
TEMPLATE = '''libos.entrypoint = "/{{ prog }}"
loader.log_level = "warning"
loader.env.LD_LIBRARY_PATH = "/lib"
loader.argv = [{{ argv }}]
fs.mounts = [
  { path = "/lib", uri = "file:{{ gramine.runtimedir() }}" },
  { path = "/{{ prog }}", uri = "file:{{ prog }}" },
]
'''


def build(work, name, source):
    (work / f"{name}.c").write_text(source)
    subprocess.run(["gcc", "-O2", "-o", str(work / name), str(work / f"{name}.c")], check=True)
    return work / name


def run_target(work, name, binary, args):
    """Profile natively, then run under saguaro-direct. Returns the comparison."""
    profile = tool.do_profile([str(binary), *args])
    matrix, evidence = tool.load_matrix()
    result = tool.check_loader(profile, "direct", matrix, evidence, tool.load_rules(), 0.5)
    predicted = {e["syscall"] for e in result["blockers"] + result["notes"]
                 if e["why"].startswith("ENOSYS in the LibOS")}
    run_dir = work / f"run-{name}"
    run_dir.mkdir()
    prog = binary.name  # busybox picks its applet from its own name
    shutil.copy(binary, run_dir / prog)
    argv = ", ".join('"%s"' % a for a in ["/" + prog, *args])
    (run_dir / f"{prog}.manifest.template").write_text(TEMPLATE)
    subprocess.run(["gramine-manifest", "-Dargv=" + argv, "-Dprog=" + prog,
                    f"{prog}.manifest.template", f"{prog}.manifest"], cwd=run_dir, check=True)
    run = subprocess.run(["saguaro-direct", prog], cwd=run_dir, capture_output=True, text=True,
                         timeout=120)
    log = run.stdout + run.stderr
    seen = set(re.findall(r"Unsupported system call (\w+)", log))
    return {"name": name, "exit": run.returncode, "predicted": predicted, "seen": seen,
            "log": log, "calls": sum(c["count"] for c in profile["calls"].values())}


def main():
    work = pathlib.Path(tempfile.mkdtemp(prefix="runtime-check-"))
    targets = [("hello", build(work, "hello", HELLO_C), []),
               ("control", build(work, "control", CONTROL_C), [])]
    busybox = shutil.which("busybox")
    if busybox:
        targets.append(("busybox-uname", pathlib.Path(busybox), ["uname", "-m"]))
    bad = 0
    for name, binary, args in targets:
        r = run_target(work, name, binary, args)
        agree = r["predicted"] == r["seen"]
        bad += not agree
        print(f"{name}: saguaro-direct exit {r['exit']}, {r['calls']} calls traced natively")
        print(f"   profile says ENOSYS in the LibOS: {sorted(r['predicted'])}")
        print(f"   LibOS reported unsupported:       {sorted(r['seen'])}   "
              f"{'agree' if agree else 'DIFFER'}")
        if not agree or r["exit"]:
            print("   log tail:", " | ".join(r["log"].strip().splitlines()[-6:])[:600])
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
