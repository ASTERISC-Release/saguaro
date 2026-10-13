#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-or-later
"""Run scripts/syscall-tolerance.py on the ten calibration applications: each is run natively with
the syscalls the direct loader reports as ENOSYS answered by -ENOSYS, and compared with an
undisturbed run. Same setup and caveats as test/calibrate-syscall-profile.py (distro builds)."""

import importlib.util
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("calibrate", ROOT / "test/calibrate-syscall-profile.py")
calibrate = importlib.util.module_from_spec(spec)
spec.loader.exec_module(calibrate)


def main():
    loader = sys.argv[1] if len(sys.argv) > 1 else "direct"
    calibrate.prepare()
    summary = []
    for name, app in calibrate.APPS.items():
        cmd = [sys.executable, str(ROOT / "scripts/syscall-tolerance.py"), "--loader", loader,
               "--wait", "2.5"]
        if app.get("drive"):
            cmd += ["--drive", app["drive"]]
        run = subprocess.run(cmd + ["--", *app["cmd"]], capture_output=True, text=True)
        print(f"=== {name}\n{run.stdout.rstrip()}", flush=True)
        summary.append((name, {0: "TOLERATED", 1: "BREAKS", 2: "baseline failed"}.get(run.returncode, "error")))
    print("\nsummary (" + loader + " loader's ENOSYS list denied):")
    for name, result in summary:
        print(f"  {name:<11} {result}")


if __name__ == "__main__":
    main()
