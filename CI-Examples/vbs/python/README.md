# Python (VBS enclave)

A small Python script (`hello.py`) that prints the Python version, a SHA-256 digest and the result
of three threads. The check looks for `Python 3.12.7`. The example is for the VBS backend only. For the
full Python example see `../../python/README.md`.

```sh
make VBS=1
gramine-vbs python
```

## Running under each backend

VBS only: `make VBS=1` builds, `make VBS=1 check` runs it and checks the output. The other backends do not apply (`make` without `VBS=1` says so). Status: matrix in [`CI-Examples/README.md`](../../README.md).
