# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The ASTERISC Research Lab

trap 'echo got USR1' USR1
kill -USR1 $$
echo after-self
( trap 'echo child got USR2; exit 5' USR2; while :; do sleep 0.05; done ) &
c=$!
sleep 0.5
kill -USR2 $c
wait $c
echo child-status=$?
sleep 3 &
kill -TERM $!
wait $!
echo term-status=$?
