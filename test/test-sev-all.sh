#!/bin/sh
exec "$(dirname "$(readlink -f "$0")")/sev/test-sev-all.sh" "$@"
