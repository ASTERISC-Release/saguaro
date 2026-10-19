# Hello World (VBS enclave)

A program that prints `Hello, world from a VBS enclave`, its process id and the number of its
arguments. The example is for the VBS backend only. For the same program on the other backends see
`../../helloworld/README.md`.

```sh
make VBS=1
gramine-vbs helloworld
```

## Running under each backend

VBS only: `make VBS=1` builds, `make VBS=1 check` runs it and checks the output. The other backends do not apply (`make` without `VBS=1` says so). Status: matrix in [`CI-Examples/README.md`](../../README.md).
