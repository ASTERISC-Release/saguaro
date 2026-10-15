#!/bin/sh

CFLAGS='-O2'

# extra flags from the subproject option `extra_cflags` (empty unless -Dvbs=enabled)
CFLAGS="$CFLAGS${MBEDTLS_EXTRA_CFLAGS:+ $MBEDTLS_EXTRA_CFLAGS}"

export CFLAGS
exec "$(dirname "$0")"/compile.sh "$@"
