#!/bin/sh
# Copyright (C) 2026 The ASTERISC Research Lab
# Saguaro x64 phase-0 spike (step h): build the x86_64 Linux program with the GNU toolchain (WSL
# Ubuntu on the x64 machine, or any x86_64 gcc) and write elf_blob.c, which places its two segments
# into sections of the enclave image (MSVC has no INCBIN in ml64).
set -e
cd "$(dirname "$0")"
gcc -O2 -fPIE -ffreestanding -fno-stack-protector -fno-asynchronous-unwind-tables -fvisibility=hidden \
    -fcf-protection=none -nostdlib -c app.c -o app.o
# DATA_ADDR: distance from .gxelft to .gxelfd in the enclave image (dumpbin /headers in build.cmd)
DATA_ADDR=${1:-0x10000}
ld -static -nostdlib --no-dynamic-linker --defsym=DATA_ADDR=$DATA_ADDR -T app.ld -o app.elf app.o
readelf -h -l app.elf | grep -E 'Machine|Entry|LOAD|Type:'
objdump -d app.elf | grep -c -E '\ssyscall' | sed 's/^/syscall instructions: /'
python3 elfsplit.py app.elf
