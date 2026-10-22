#!/bin/sh

# Copyright (C) 2023 Gramine contributors
# SPDX-License-Identifier: BSD-3-Clause

set -e

# GRAMINE (for example saguaro-vm, from exec.sh) names the runner; otherwise SGX selects saguaro-sgx
if test -z "$GRAMINE"
then
    if test -n "$SGX"
    then
        GRAMINE=saguaro-sgx
    else
        GRAMINE=saguaro-direct
    fi
fi

for i in `ls "$DATA_DIR"/scenes/`;
do
    rm -f "$DATA_DIR"/images/"$i"0001.png
    $GRAMINE "../blender" -b /data/scenes/$i -t 4 -F PNG -o /data/images/$i -f 1
    # TODO add a better test, probably some diff with a precomputed image
    [ -f "$DATA_DIR"/images/"$i"0001.png ]
done
