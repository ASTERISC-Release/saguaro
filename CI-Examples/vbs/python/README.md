# Python (VBS enclave)

A small Python script (`hello.py`) that prints the Python version, a SHA-256 digest and the result
of three threads. The example is for the VBS backend only. For the full Python example see
`../../python/README.md`. Pass or fail status: matrix in
[`CI-Examples/README.md`](../../README.md).

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

The check looks for `Python 3.12.7` and prints `[ Success ]`. To run the script by hand:

```sh
gramine-vbs python
```
