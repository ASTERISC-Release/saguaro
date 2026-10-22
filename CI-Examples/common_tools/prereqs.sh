# SPDX-License-Identifier: BSD-3-Clause
# Copyright (C) 2026 The ASTERISC Research Lab
# Shared helpers for the per-example `install-prereqs.sh` scripts (source this file).
#
# Behaviour: idempotent; only installs what is missing; uses `sudo` (may prompt for a password)
# unless already root. Set DRY_RUN=1 to print what would be installed without doing it.
# Debian/Ubuntu only (apt); on other distros the scripts print the package list instead.

PREREQ_APT_UPDATED=0

prereq_log()  { echo "[prereqs] $*"; }
prereq_die()  { echo "[prereqs] ERROR: $*" >&2; exit 1; }

prereq_sudo() {
    if [ "$(id -u)" = 0 ]; then "$@"; else sudo "$@"; fi
}

# apt_install pkg...   -- installs the subset of packages that is not already installed
apt_install() {
    local missing=() p
    for p in "$@"; do
        [ -n "$p" ] || continue
        dpkg -s "$p" >/dev/null 2>&1 || missing+=("$p")
    done
    if [ ${#missing[@]} -eq 0 ]; then
        prereq_log "apt packages already installed: $*"
        return 0
    fi
    if ! command -v apt-get >/dev/null; then
        prereq_die "apt-get not found; please install manually: ${missing[*]}"
    fi
    prereq_log "installing apt packages: ${missing[*]}"
    if [ "${DRY_RUN:-0}" = 1 ]; then
        prereq_log "(dry run) sudo apt-get install -y ${missing[*]}"
        return 0
    fi
    if [ "$PREREQ_APT_UPDATED" = 0 ]; then
        prereq_sudo apt-get update || prereq_die "apt-get update failed"
        PREREQ_APT_UPDATED=1
    fi
    prereq_sudo env DEBIAN_FRONTEND=noninteractive apt-get install -y "${missing[@]}" \
        || prereq_die "apt-get install failed"
}

# first_available_apt pkg...  -- prints the first package that apt knows a candidate for
first_available_apt() {
    local p out
    for p in "$@"; do
        # Capture first: `apt-cache ... | grep -q` can die of SIGPIPE, which fails under pipefail.
        out=$(apt-cache policy "$p" 2>/dev/null || true)
        if [[ "$out" == *"Candidate: "* && "$out" != *"Candidate: (none)"* ]]; then echo "$p"; return 0; fi
    done
    return 1
}

# pip_user_install module pkg...  -- `pip3 install --user` pkg unless `import module` works
pip_user_install() {
    local mod=$1; shift
    if python3 -c "import $mod" >/dev/null 2>&1; then
        prereq_log "python module '$mod' already available"
        return 0
    fi
    prereq_log "pip installing: $*"
    [ "${DRY_RUN:-0}" = 1 ] && { prereq_log "(dry run) pip3 install --user $*"; return 0; }
    pip3 install --user --break-system-packages "$@" 2>/dev/null \
        || pip3 install --user "$@" || prereq_die "pip install failed: $*"
}

# Warn (do not fail) if Gramine itself is not installed; examples need saguaro-direct.
check_gramine() {
    if command -v saguaro-direct >/dev/null; then
        prereq_log "saguaro-direct found: $(command -v saguaro-direct)"
    else
        prereq_log "WARNING: saguaro-direct not on PATH. Build and install Gramine first" \
                   "(gramine-tdx/install.sh) and add <prefix>/bin to PATH."
    fi
}
