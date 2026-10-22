# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The ASTERISC Research Lab
#
# Shared environment for the test/benchmark/stress scripts (source this file; do not execute it).
#
# Inputs (environment):
#   SAGUARO_PREFIX   install prefix of the Saguaro build under test (default: ~/.local/saguaro)
#   LOGDIR           where scripts write per-test logs (default: ${TMPDIR:-/tmp}/saguaro-tests)
#
# Effects: puts $SAGUARO_PREFIX/bin on PATH and the prefix's python site-packages on PYTHONPATH
# (whichever python3.X directory exists, preferring the running python3's version), so the
# scripts work with any Python version and any install prefix.

SAGUARO_PREFIX="${SAGUARO_PREFIX:-$HOME/.local/saguaro}"
export SAGUARO_PREFIX
export PATH="$SAGUARO_PREFIX/bin:$PATH"

_gramine_py_ver=$(python3 -c 'import sys; print("%d.%d" % sys.version_info[:2])' 2>/dev/null || true)
_gramine_sp=""
for _d in "$SAGUARO_PREFIX/lib/python${_gramine_py_ver}/site-packages" \
          "$SAGUARO_PREFIX/lib/python${_gramine_py_ver}/dist-packages" \
          "$SAGUARO_PREFIX"/lib/python3*/site-packages \
          "$SAGUARO_PREFIX"/lib/python3*/dist-packages; do
    if [ -d "$_d" ]; then _gramine_sp=$_d; break; fi
done
if [ -n "$_gramine_sp" ]; then
    export PYTHONPATH="$_gramine_sp${PYTHONPATH:+:$PYTHONPATH}"
fi
unset _gramine_py_ver _gramine_sp _d

export LOGDIR="${LOGDIR:-${TMPDIR:-/tmp}/saguaro-tests}"
