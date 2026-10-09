#!/usr/bin/env bash
#
# install.sh — build and install the Gramine (Tirith fork) "direct" PAL,
# following the same steps documented in run_all_tests.sh's header, and
# optionally run the test battery afterwards.
#
# Usage:
#   ./install.sh                     # deps info + meson setup + build + install
#   ./install.sh --deps              # force the apt dependency install (done
#                                    # automatically if build tools are missing)
#   ./install.sh --run-tests         # run ./run_all_tests.sh --no-build after
#   ./install.sh --run-tests libos   # ...limited to the named suites
#   ./install.sh --prefix /opt/gramine --build-dir build-x
#   ./install.sh --sgx | --vm | --tdx    # enable extra PAL backends (off by
#                                    # default; "direct" is the default build)
#   ./install.sh --secure-gaming disabled  # build the LibOS without the Tirith
#                                    # secure-gaming (SG) host-redirection code
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
#  * Dependencies are installed via apt (sudo, may prompt for a password)
#    automatically when build tools are missing, or always with --deps.
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
SG=enabled   # Tirith secure-gaming paths in the LibOS (see meson_options.txt)
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
        --secure-gaming) SG=$2; shift 2 ;;
        -j*)          JOBS=${1#-j}; [ -n "$JOBS" ] || { JOBS=$2; shift; }; shift ;;
        pal|libos|helloworld|multithread|busybox|redis|memcached)
                      [ "$RUN_TESTS" = 1 ] || RUN_TESTS=1
                      TEST_SUITES+=("$1"); shift ;;
        *)            echo "unknown option: $1 (see --help)"; exit 2 ;;
    esac
done

APT_PKGS='build-essential meson ninja-build pkg-config python3-pytest
    python3-click python3-jinja2 python3-cryptography python3-voluptuous
    python3-toml python3-tomli python3-tomli-w python3-pip bison gawk nasm procps iproute2 file linux-libc-dev'

missing_tools() {
    for tool in meson ninja python3 cc make pkg-config bison; do
        command -v "$tool" >/dev/null || return 0
    done
    return 1
}

# Install dependencies when asked (--deps) or when build tools are missing.
# sudo may prompt for a password interactively.
if [ "$DO_APT" = 1 ] || missing_tools; then
    echo ">>> installing dependencies via apt (sudo may ask for your password)"
    SUDO=sudo; [ "$(id -u)" = 0 ] && SUDO=
    $SUDO apt-get update
    # shellcheck disable=SC2086
    $SUDO apt-get install -y $APT_PKGS
fi

for tool in meson ninja python3 cc make bison; do
    command -v "$tool" >/dev/null || { echo "ERROR: '$tool' not found after dependency install"; exit 1; }
done

echo ">>> configuring ($BUILDDIR -> $PREFIX, sgx=$SGX vm=$VM tdx=$TDX)"
# On an already-configured build dir this reconfigures in place (cheap);
# --reconfigure covers the case where meson insists on regeneration.
if ! meson setup "$BUILDDIR" \
        --prefix="$PREFIX" \
        -Dbuildtype=release \
        -Ddirect=enabled \
        -Dsgx="$SGX" -Dvm="$VM" -Dtdx="$TDX" -Dsecure_gaming="$SG" \
        -Dtests=enabled \
        -Dlibc=glibc 2>"$BUILDDIR-setup.err"; then
    echo "    plain 'meson setup' failed; retrying with --reconfigure"
    meson setup --reconfigure "$BUILDDIR" \
        --prefix="$PREFIX" \
        -Dbuildtype=release \
        -Ddirect=enabled \
        -Dsgx="$SGX" -Dvm="$VM" -Dtdx="$TDX" -Dsecure_gaming="$SG" \
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
