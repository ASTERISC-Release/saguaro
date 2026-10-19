# PyTorch

This directory contains steps and artifacts to run a PyTorch sample workload on Gramine. The
example uses a pre-trained model for image classification. Pass or fail status per backend: matrix
in [`CI-Examples/README.md`](../README.md).

# Building

Install the backend first (`Documentation/quickstart/`) and run all commands in this directory.

## Pre-requisites

On a standard Ubuntu distribution:

```sh
sudo apt install libnss-mdns libnss-myhostname python3-pip lsb-release
```

The first two packages are DNS-resolver libraries. `pip` installs the Python packages, and the
Makefile uses `lsb_release`.

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

The workload reads `input.jpg` and prints what the classifier detects:

```sh
gramine-direct ./pytorch ./pytorchexample.py
```

For the image that ships with the example, the output is:

```
[('Labrador retriever', 41.58518600463867), ('golden retriever', 16.59165382385254), ('Saluki, gazelle hound', 16.286855697631836), ('whippet', 2.853914976119995), ('Ibizan hound, Ibizan Podenco', 2.3924756050109863)]
```

The other backends use the same arguments (SGX: not verified):

```sh
gramine-vm ./pytorch ./pytorchexample.py
gramine-sev ./pytorch ./pytorchexample.py
gramine-sgx ./pytorch ./pytorchexample.py    # SGX
```

`python3 pytorchexample.py` runs the workload without Gramine.
