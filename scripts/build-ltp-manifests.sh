#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# Prepare all LTP syscall manifests with the repository's upstream manifest template.
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
prefix=${SAGUARO_PREFIX:-"$HOME/.local/saguaro-vm"}
ltp_root=${LTP_PREFIX:-"$repo_root/.ltp-install"}
test_dir="$repo_root/libos/test/ltp"
# LTP_HARNESS_DIR: build the manifests in another directory, a copy of libos/test/ltp's harness
# files, for a second Saguaro install whose manifests differ (on aarch64 the direct PAL and the vm
# PAL are separate installs, each rendering its own libsysdb.so path into the manifests).
harness_dir=${LTP_HARNESS_DIR:-"$test_dir"}
if [[ "$(realpath -m "$harness_dir")" != "$test_dir" ]]; then
    mkdir -p "$harness_dir"
    cp "$test_dir"/{manifest.template,tests.toml,test_ltp.py,ltp.cfg,ltp_all.cfg,ltp_sgx.cfg} "$harness_dir"/
    test_dir=$(realpath "$harness_dir")
fi

[[ -x "$ltp_root/testcases/bin/getpid01" ]] || {
    echo "LTP not installed under $ltp_root; run scripts/fetch-ltp.sh first" >&2
    exit 1
}
[[ -x "$prefix/bin/saguaro-test" ]] || {
    echo "Saguaro test tools not installed under $prefix" >&2
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
LTP_CONFIG=ltp_all.cfg saguaro-test build

# The splice02 scenario is a pipeline, so upstream's manifest builder does not discover
# its test binary. Its sibling uses the same template and runtime files.
if [[ -f splice01.manifest && ! -f splice02.manifest ]]; then
    sed 's/splice01/splice02/g' splice01.manifest > splice02.manifest
fi

# The VM PAL has no dev: stream, but LibOS tmpfs can provide a private /dev/shm for the
# single-process VM test run. Keep the default (host /dev/shm, shared between the processes of a
# test) for native/direct/SGX runs: LTP's library keeps its result counters in a /dev/shm mapping
# shared with the forked test process. scripts/run-ltp-syscalls.py switches the manifests it runs
# to the variant its loader needs, so this setting only chooses the initial state.
if [[ ${LTP_VM_TMPFS_SHM:-0} == 1 ]]; then
    python3 "$repo_root/scripts/run-ltp-syscalls.py" --set-shm tmpfs --harness-dir "$test_dir"
else
    python3 "$repo_root/scripts/run-ltp-syscalls.py" --set-shm host --harness-dir "$test_dir"
fi

# Process-syscall measurements must opt in explicitly while the VM PAL implementation is
# experimental. Set or clear this option even in unchanged generated manifests, because
# saguaro-test build leaves up-to-date manifest files alone.
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
