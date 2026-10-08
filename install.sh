#!/usr/bin/env bash
#
# install.sh — build and install the Gramine (Tirith fork) "direct" PAL,
# following the same steps documented in run_all_tests.sh's header, and
# optionally run the test battery afterwards.
#
# Usage:
#   ./install.sh                     # deps info + meson setup + build + install
#   ./install.sh --deps              # additionally run the apt dependency line
#                                    # (needs sudo; skipped with a notice if
#                                    #  sudo is unavailable)
#   ./install.sh --run-tests         # run ./run_all_tests.sh --no-build after
#   ./install.sh --run-tests libos   # ...limited to the named suites
#   ./install.sh --prefix /opt/gramine --build-dir build-x
#   ./install.sh --sgx | --vm | --tdx    # enable extra PAL backends (off by
#                                    # default; "direct" is the default build)
#   ./install.sh -j 8                # ninja parallelism (default: nproc)
#   ./install.sh --help
#
# Defaults match this repo's tested configuration:
#   prefix    $HOME/.local/gramine      (private, no root needed)
#   build dir build-$(uname -m)         (aarch64 -> build-aarch64 etc.)
#   -Ddirect=enabled -Dsgx=disabled -Dvm=disabled -Dtdx=disabled
#   -Dtests=enabled -Dlibc=glibc -Dbuildtype=release
#
# Re-running is cheap and safe: meson reuses a configured build dir with
# matching options and ninja rebuilds only what changed.
#
# Notes:
#  * --deps is OFF by default because this box has no passwordless sudo; the
#    exact apt line is printed instead so you can run it yourself.
#  * vm/tdx are Tirith forks of the PAL that need extra system dependencies
#    (and SGX/TDX hardware); they stay disabled for the direct-PAL battery.
#  * nasm is only needed with tests=enabled on x86_64.

set -euo pipefail

PREFIX="$HOME/.local/gramine"
BUILDDIR="build-$(uname -m)"
DO_APT=0
RUN_TESTS=0
JOBS="$(nproc)"
SGX=disabled VM=disabled TDX=disabled
TEST_SUITES=()

script_dir=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)

usage() { sed -n '/^# install.sh/,/^# Notes:/p' "$0" | sed 's/^# \{0,1\}//'; }

while [ $# -gt 0 ]; do
    case "$1" in
        -h|--help)    usage; exit 0 ;;
        --prefix)     PREFIX=$2; shift 2 ;;
        --build-dir)  BUILDDIR=$2; shift 2 ;;
        --deps)       DO_APT=1; shift ;;
        --run-tests)  RUN_TESTS=1; shift ;;
        --sgx)        SGX=enabled; shift ;;
        --vm)         VM=enabled; shift ;;
        --tdx)        TDX=enabled; shift ;;
        -j*)          JOBS=${1#-j}; [ -n "$JOBS" ] || { JOBS=$2; shift; }; shift ;;
        pal|libos|helloworld|multithread|busybox|redis|memcached)
                      [ "$RUN_TESTS" = 1 ] || RUN_TESTS=1
                      TEST_SUITES+=("$1"); shift ;;
        *)            echo "unknown option: $1 (see --help)"; exit 2 ;;
    esac
done

APT_LINE='sudo apt install build-essential meson ninja-build pkg-config \
    python3-pytest python3-click python3-jinja2 python3-cryptography \
    python3-voluptuous python3-toml nasm procps iproute2 file linux-libc-dev'

for tool in meson ninja python3 cc; do
    command -v "$tool" >/dev/null || { echo "ERROR: '$tool' not found (needed for the build; see --deps)"; exit 1; }
done

if [ "$DO_APT" = 1 ]; then
    if sudo -n true 2>/dev/null || [ "$(id -u)" = 0 ]; then
        echo ">>> installing dependencies via apt"
        eval "$APT_LINE"
    else
        echo ">>> --deps requested but passwordless sudo is unavailable;"
        echo "    run this yourself:"
        echo "    $APT_LINE"
    fi
fi

echo ">>> configuring ($BUILDDIR -> $PREFIX, sgx=$SGX vm=$VM tdx=$TDX)"
# On an already-configured build dir this reconfigures in place (cheap);
# --reconfigure covers the case where meson insists on regeneration.
if ! meson setup "$BUILDDIR" \
        --prefix="$PREFIX" \
        -Dbuildtype=release \
        -Ddirect=enabled \
        -Dsgx="$SGX" -Dvm="$VM" -Dtdx="$TDX" \
        -Dtests=enabled \
        -Dlibc=glibc 2>"$BUILDDIR-setup.err"; then
    echo "    plain 'meson setup' failed; retrying with --reconfigure"
    meson setup --reconfigure "$BUILDDIR" \
        --prefix="$PREFIX" \
        -Dbuildtype=release \
        -Ddirect=enabled \
        -Dsgx="$SGX" -Dvm="$VM" -Dtdx="$TDX" \
        -Dtests=enabled \
        -Dlibc=glibc \
    || { echo "ERROR: meson setup failed:"; tail -5 "$BUILDDIR-setup.err"; exit 1; }
fi
rm -f "$BUILDDIR-setup.err"

echo ">>> building (-j$JOBS)"
ninja -C "$BUILDDIR" -j "$JOBS"

echo ">>> installing to $PREFIX"
ninja -C "$BUILDDIR" -j "$JOBS" install

if [ "$RUN_TESTS" = 1 ]; then
    echo ">>> running the test battery"
    exec "$script_dir/run_all_tests.sh" --no-build ${TEST_SUITES[@]+"${TEST_SUITES[@]}"}
fi

echo
echo "Done. Test with:  $script_dir/run_all_tests.sh"
echo "(or rerun as:    ./install.sh --run-tests)"
