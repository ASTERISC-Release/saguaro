# Python (VBS enclave)

Runs `hello.py`: prints the Python version, a SHA-256 digest and the result of three threads. VBS only; the full Python example is `../../python/`.

## Run

In WSL on a Windows machine with VBS enabled. Install the backend first: [`Documentation/quickstart/vbs-windows.md`](../../../Documentation/quickstart/vbs-windows.md) (machine setup: [`vbs-enable.md`](../../../Documentation/quickstart/vbs-enable.md)).

```sh
make VBS=1              # build and sign
make VBS=1 check        # run and check
saguaro-vbs python      # by hand
```

`../exec.sh --vbs` builds, runs and checks all the VBS examples; `make` without `VBS=1` says that the example does not run elsewhere.
