#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# Download and build the LTP revision formerly pinned by libos/test/ltp/src.
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
source_dir=${LTP_SOURCE_DIR:-"$repo_root/.ltp-src"}
prefix=${LTP_PREFIX:-"$repo_root/.ltp-install"}
revision=da2f34028f046a208aa2fed5e287df2538e69f91

if [[ ! -d "$source_dir/.git" ]]; then
    git clone --no-checkout https://github.com/linux-test-project/ltp.git "$source_dir"
fi
git -C "$source_dir" fetch --depth 1 origin "$revision"
git -C "$source_dir" checkout --detach "$revision"

cd "$source_dir"
if [[ ! -x ./configure ]]; then
    make autotools
fi
./configure --prefix="$prefix"
make -j"${LTP_JOBS:-4}"
make install
printf 'LTP %s installed under %s\n' "$revision" "$prefix"
