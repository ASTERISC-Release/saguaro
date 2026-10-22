#!/bin/bash
# Copyright (C) 2026 The ASTERISC Research Lab
# Saguaro x64 phase-0 spike: what an x86_64 ELF needs from a VBS packer. Usage: elf-scan.sh <elf>
# Prints relocation types, raw system-call instructions (syscall, sysenter, int 0x80), %fs/%gs-relative
# instructions, red-zone uses (negative %rsp displacements), xsave/cpuid/rdtsc counts.
f=$1
dis=$(mktemp)
trap 'rm -f "$dis"' EXIT
echo "== $f"
file -b "$f" | cut -c1-100
echo "relocations:"; readelf -rW "$f" | awk '/R_X86_64/{print $3}' | sort | uniq -c
objdump -d --no-show-raw-insn "$f" > "$dis"
printf "syscall: %s  sysenter: %s  int0x80: %s\n" $(grep -cE '\ssyscall\b' "$dis") $(grep -cE '\ssysenter\b' "$dis") $(grep -cE '\sint\s+\$0x80' "$dis")
printf "fs-relative insns: %s  gs-relative: %s  rd/wrfsbase: %s\n" $(grep -c '%fs:' "$dis") $(grep -c '%gs:' "$dis") $(grep -cE 'r[dw][fg]sbase' "$dis")
printf "red-zone uses (negative rsp displacement): %s in %s functions\n" $(grep -cE -e "-0x[0-9a-f]+\(%rsp\)" "$dis") $(awk '/^[0-9a-f]+ <.*>:$/{fn=$2} /-0x[0-9a-f]+\(%rsp\)/{print fn}' "$dis" | sort -u | wc -l)
printf "xsave/xrstor/fxsave: %s  cpuid: %s  rdtsc: %s\n" $(grep -cE '\s(xsave|xrstor|fxsave|fxrstor)' "$dis") $(grep -cE '\scpuid\b' "$dis") $(grep -cE '\srdtscp?\b' "$dis")
echo "syscall sites by function (top):"; awk '/^[0-9a-f]+ <.*>:$/{fn=$2} /[[:space:]]syscall *$/{print fn}' "$dis" | sort | uniq -c | sort -rn | head -5
echo "fs-relative operand forms (top):"; grep -oE '%fs:[^,]*' "$dis" | sed -E 's/0x[0-9a-f]+/N/g' | sort | uniq -c | sort -rn | head -6
