#!/bin/sh
exec "$(dirname "$(readlink -f "$0")")/vm/test-vm.sh" "$@"
