#!/bin/sh
exec "$(dirname "$(readlink -f "$0")")/direct/test-direct.sh" "$@"
