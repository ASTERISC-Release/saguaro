#!/bin/sh
# Gramine phase-0 spike (step h): build the AArch64 Linux program with the GNU toolchain (WSL
# Ubuntu 24.04 on the ARM64 machine, native aarch64 gcc) and split it into the two binaries that
# ../elf.asm includes into the enclave image.
set -e
cd "$(dirname "$0")"
gcc -O2 -fPIE -ffreestanding -fno-stack-protector -fno-asynchronous-unwind-tables -fvisibility=hidden \
    -ffixed-x18 -nostdlib -c app.c -o app.o
# DATA_ADDR: distance from .gxelft to .gxelfd in the enclave image (dumpbin /headers in build.cmd)
DATA_ADDR=${1:-0x1E000}
ld -static -nostdlib --defsym=DATA_ADDR=$DATA_ADDR -T app.ld -o app.elf app.o
readelf -h -l app.elf | grep -E 'Machine|Entry|LOAD|Type:'
objdump -d app.elf | sed -n '/<_start>:/,$p'
python3 elfsplit.py app.elf
