# Bash

Runs Bash and the commands and scripts it starts (`ls`, the scripts in `scripts/`).

## Run

Install the backend first: [`Documentation/quickstart/`](../../Documentation/quickstart/README.md).

```sh
./exec.sh --direct      # builds, runs and checks; also: --vm, --sev, --sev-snp, --sgx
```

By hand:

```sh
make
gramine-direct ./bash -c "ls"
make regression         # 7 checks, prints [ Success N/7 ]; GRAMINE=gramine-vm etc. for another backend, SGX=1 for SGX
```

## Notes

- The checks start child processes, so the manifest sets `sys.experimental__multiprocess = true` (used by the VM, x86_64 only).
