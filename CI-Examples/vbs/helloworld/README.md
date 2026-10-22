# Hello World (VBS enclave)

Prints `Hello, world from a VBS enclave`, its process id and its argument count. VBS only; the same program for the other backends is in `../../helloworld/`.

## Run

In WSL on a Windows machine with VBS enabled. Install the backend first: [`Documentation/quickstart/vbs-windows.md`](../../../Documentation/quickstart/vbs-windows.md) (machine setup: [`vbs-enable.md`](../../../Documentation/quickstart/vbs-enable.md)).

```sh
make VBS=1              # build and sign
make VBS=1 check        # run and check
saguaro-vbs helloworld  # by hand
```

`../exec.sh --vbs` builds, runs and checks all the VBS examples; `make` without `VBS=1` says that the example does not run elsewhere.
