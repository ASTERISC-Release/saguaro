# Dynamically linked program (VBS enclave)

A dynamically linked program. The LibOS loads it and `ld.so`, and `ld.so` maps `libc.so.6`. The
program sorts an array with `qsort` and prints the result; `make VBS=1 check` looks for the
lines `sorted 1 3 5 7 9` and `cos(0) = 1`. The example is for the VBS backend only.

```sh
make VBS=1
gramine-vbs dynhello
```

## Running under each backend

VBS only: `make VBS=1` builds, `make VBS=1 check` runs it and checks the output. The other backends do not apply (`make` without `VBS=1` says so). Status: matrix in [`CI-Examples/README.md`](../../README.md).
