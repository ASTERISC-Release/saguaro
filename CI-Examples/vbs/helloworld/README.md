# Hello World (VBS enclave)

A program that prints `Hello, world from a VBS enclave`, its process id and the number of its
arguments. The example is for the VBS backend only. For the same program on the other backends see
`../../helloworld/README.md`. Pass or fail status: matrix in
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

The check expects the line `Hello, world from a VBS enclave (pid N, 0 arguments)` and prints
`[ Success ]`. To run the program by hand:

```sh
gramine-vbs helloworld
```
