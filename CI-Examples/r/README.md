# R

Runs the R interpreter on `scripts/sample.r`.

## Prerequisites

```sh
./install-prereqs.sh    # installs r-base-core (R_HOME defaults to /usr/lib/R)
```

## Run

Install the backend first: [`Documentation/quickstart/`](../../Documentation/quickstart/README.md).

```sh
./exec.sh --direct      # builds, runs and checks; also: --vm, --sev, --sev-snp, --sgx
```

By hand:

```sh
make                    # make SGX=1 for SGX; for another R: make R_HOME=<install path>/lib/R
make check              # runs scripts/sample.r, prints [ Success 1/1 ]; GRAMINE=gramine-vm etc., SGX=1 for SGX
gramine-direct ./R --slave --vanilla -f scripts/R-benchmark-25.R
```

## Notes

- R needs `--vanilla` or `--no-save` under Gramine.
