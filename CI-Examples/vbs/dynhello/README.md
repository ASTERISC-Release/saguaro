# Dynamically linked program (VBS enclave)

A dynamically linked program. The LibOS loads it and `ld.so`, and `ld.so` maps `libc.so.6`. The
program sorts an array with `qsort` and prints the result. The example is for the VBS backend only.
Pass or fail status: matrix in [`CI-Examples/README.md`](../../README.md).

# Building

Install the VBS backend first (`Documentation/vbs.md`) and run all commands in this directory.

## VBS

```sh
make VBS=1
```

The example does not run under direct, VM or SEV; `make` without `VBS=1` says so.

# Testing

```sh
make VBS=1 check
```

The check looks for the lines `sorted 1 3 5 7 9` and `cos(0) = 1`. To run the program by hand:

```sh
gramine-vbs dynhello
```
