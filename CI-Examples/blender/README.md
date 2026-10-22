# Blender

Renders the scenes in `data/scenes/` with Blender 2.82. The Makefile downloads an x86_64 binary, so the example runs on x86_64 only.

## Prerequisites

```sh
./install-prereqs.sh    # installs lsb_release, which the Makefile uses
```

## Run

Install the backend first: [`Documentation/quickstart/`](../../Documentation/quickstart/README.md).

```sh
./exec.sh --direct      # builds, runs and checks; also: --vm, --sev, --sev-snp, --sgx
```

By hand:

```sh
make
make check              # renders every scene, expects data/images/<scene>0001.png; SAGUARO=saguaro-vm etc., SGX=1 for SGX
```
