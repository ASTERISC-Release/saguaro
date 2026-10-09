#!/bin/bash -e

# compile gramine-tdx
meson setup build-release/ --buildtype=release -Dtests=enabled \
  -Dskeleton=enabled -Ddirect=enabled -Dsgx=disabled -Dvm=enabled -Dtdx=disabled \
  --prefix=$PWD/built-debug --reconfigure

ninja -C build-release/
ninja -C build-release/ install

echo "Gramine-tdx installed successfully"
