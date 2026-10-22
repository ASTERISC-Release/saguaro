#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-or-later
"""Run every LTP test of one syscall under one loader (native, direct or vm).

The test list comes from dev docs: syscall-compat/mapping.toml; the runs are done by
scripts/run-ltp-syscalls.py. Use it inside the container with the install's env.sh sourced (see
dev docs: syscall-compat/RERUN.md). Extra arguments after `--` go to the runner.
"""

import argparse
import os
import pathlib
import sys
import tomllib

ROOT = pathlib.Path(__file__).resolve().parents[1]
# The syscall-compat and syscall-profile data live in the private development-documents repository, checked out
# next to this one (SAGUARO_DEV_DOCS names another place).
DEV_DOCS = pathlib.Path(os.environ.get("SAGUARO_DEV_DOCS", ROOT.parent / "gramine-multiarch-docs"))
MAPPING = DEV_DOCS / "syscall-compat/mapping.toml"


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("syscall", help="syscall name, for example mmap or accept4")
    parser.add_argument("--loader", choices=("native", "direct", "vm"), default="vm")
    parser.add_argument("--list", action="store_true", help="only print the tests")
    parser.add_argument("--ltp-root", type=pathlib.Path,
                        help="LTP install (default: .ltp-install; .ltp-nofork-install for vm)")
    parser.add_argument("runner_args", nargs="*", help="after --: passed to run-ltp-syscalls.py")
    args = parser.parse_args()

    with open(MAPPING, "rb") as f:
        tests = [t["name"] for t in tomllib.load(f)["test"] if args.syscall in t["syscalls"]]
    if not tests:
        sys.exit(f"no LTP test is mapped to '{args.syscall}' (see {MAPPING.name})")
    if args.list:
        print("\n".join(tests))
        return

    nofork = ROOT / ".ltp-nofork-install"
    root = args.ltp_root or (nofork if args.loader == "vm" and nofork.is_dir()
                             else ROOT / ".ltp-install")
    cmd = [sys.executable, str(ROOT / "scripts/run-ltp-syscalls.py"), "--loader", args.loader,
           "--ltp-root", str(root), "--jobs", "1", "--timeout-factor", "2",
           "--results", str(ROOT / f".ltp-results/{args.syscall}-{args.loader}.json"),
           *args.runner_args, *tests]
    if args.loader == "vm":
        os.environ.setdefault("SAGUARO_CPU_NUM", "1")
    print(f"{len(tests)} tests of {args.syscall} under {args.loader}", file=sys.stderr)
    os.execv(sys.executable, cmd)


if __name__ == "__main__":
    main()
