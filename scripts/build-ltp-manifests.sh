#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# Prepare all LTP syscall manifests with the repository's upstream manifest template.
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
prefix=${GRAMINE_PREFIX:-"$HOME/.local/gramine-vm"}
ltp_root=${LTP_PREFIX:-"$repo_root/.ltp-install"}
test_dir="$repo_root/libos/test/ltp"

[[ -x "$ltp_root/testcases/bin/getpid01" ]] || {
    echo "LTP not installed under $ltp_root; run scripts/fetch-ltp.sh first" >&2
    exit 1
}
[[ -x "$prefix/bin/gramine-test" ]] || {
    echo "Gramine test tools not installed under $prefix" >&2
    exit 1
}
if [[ -e "$test_dir/install" && ! -L "$test_dir/install" ]]; then
    echo "$test_dir/install exists and is not a symlink; refusing to replace it" >&2
    exit 1
fi
ln -sfn "$(realpath --relative-to="$test_dir" "$ltp_root")" "$test_dir/install"
export PATH="$prefix/bin:$PATH"
export PYTHONPATH="$prefix/lib/python3.12/site-packages${PYTHONPATH:+:$PYTHONPATH}"
cd "$test_dir"
LTP_CONFIG=ltp_all.cfg gramine-test build

# The splice02 scenario is a pipeline, so upstream's manifest builder does not discover
# its test binary. Its sibling uses the same template and runtime files.
if [[ -f splice01.manifest && ! -f splice02.manifest ]]; then
    sed 's/splice01/splice02/g' splice01.manifest > splice02.manifest
fi

# The VM PAL has no dev: stream, but LibOS tmpfs can provide private /dev/shm for the
# single-process VM test run. Keep the default manifest behavior for native/direct/SGX runs.
if [[ ${LTP_VM_TMPFS_SHM:-0} == 1 ]]; then
    python3 - "$test_dir" <<'PY'
from pathlib import Path
import sys

root = Path(sys.argv[1])
old = '{ type = "untrusted_shm", path = "/dev/shm", uri = "dev:/dev/shm" },'
new = '{ type = "tmpfs", path = "/dev/shm" },'
changed = 0
configured = 0
for manifest in root.glob("*.manifest"):
    text = manifest.read_text()
    if old in text:
        text = text.replace(old, new)
        manifest.write_text(text)
        changed += 1
    if new in text:
        configured += 1
print(f"Configured /dev/shm as tmpfs in {configured} VM manifests ({changed} changed)")
PY
fi

# Process-syscall measurements must opt in explicitly while the VM PAL implementation is
# experimental. Set or clear this option even in unchanged generated manifests, because
# gramine-test build leaves up-to-date manifest files alone.
python3 - "$test_dir" "${LTP_VM_MULTIPROCESS:-0}" <<'PY'
from pathlib import Path
import sys

root = Path(sys.argv[1])
enabled = sys.argv[2] == "1"
changed = 0
for manifest in root.glob("*.manifest"):
    original = manifest.read_text()
    lines = [line for line in original.splitlines(keepends=True)
             if not line.startswith("experimental__multiprocess =")]
    text = "".join(lines)
    if enabled:
        marker = "[sys]\n"
        if marker in text:
            text = text.replace(marker, marker + "experimental__multiprocess = true\n", 1)
    if text != original:
        manifest.write_text(text)
        changed += 1
print(f"Set experimental VM multiprocess support to {enabled} in {changed} LTP manifests")
PY
