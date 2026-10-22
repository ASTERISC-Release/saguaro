# PyTorch

Classifies an image (`input.jpg`) with a pre-trained AlexNet model and writes the result to `result.txt`.

## Prerequisites

```sh
./install-prereqs.sh    # python3, venv, pip, lsb-release; the Makefile then installs CPU-only torch into ./my_venv (about 900 MB)
```

## Run

Install the backend first: [`Documentation/quickstart/`](../../Documentation/quickstart/README.md).

```sh
./exec.sh --direct      # builds, runs and checks; also: --vm, --sev, --sev-snp, --sgx
```

By hand:

```sh
make                    # make SGX=1 for SGX
saguaro-direct ./pytorch ./pytorchexample.py     # or saguaro-vm, saguaro-sev, saguaro-sgx
cat result.txt          # first entry: Labrador retriever
```

`python3 pytorchexample.py` runs the workload without Saguaro.
