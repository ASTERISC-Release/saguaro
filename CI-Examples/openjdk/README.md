# Java example

This directory contains an example for running OpenJDK in Gramine, including the Makefile and a
template for generating the manifest. Pass or fail status per backend: matrix in
[`CI-Examples/README.md`](../README.md).

# Building

Install the backend first (`Documentation/quickstart/`) and run all commands in this directory.
Install OpenJDK (Ubuntu):

```sh
sudo apt-get install openjdk-11-jdk
```

## Direct, VM, and SEV

```sh
make
```

## SGX

```sh
make SGX=1
```

The example has no VBS recipe.

# Testing

The program `MultiThreadMain` starts several threads:

```sh
gramine-direct java MultiThreadMain
gramine-vm java MultiThreadMain             # VM
gramine-sev java MultiThreadMain            # SEV
gramine-sgx java -Xmx8G MultiThreadMain     # SGX; not verified
```

Under SGX, `-Xmx8G` is needed unless the enclave size is 64G or more.
