# Dynamically linked program (VBS enclave)

A dynamically linked program (loaded with `ld.so` and `libc.so.6`) that sorts an array with `qsort` and prints the result. VBS only.

## Run

In WSL on a Windows machine with VBS enabled. Install the backend first: [`Documentation/quickstart/vbs-windows.md`](../../../Documentation/quickstart/vbs-windows.md) (machine setup: [`vbs-enable.md`](../../../Documentation/quickstart/vbs-enable.md)).

```sh
make VBS=1              # build and sign
make VBS=1 check        # run and check
gramine-vbs dynhello    # by hand
```

`../exec.sh --vbs` builds, runs and checks all the VBS examples; `make` without `VBS=1` says that the example does not run elsewhere.
