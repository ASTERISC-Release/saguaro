# Curl

This directory contains a Makefile and a template manifest to run curl on Gramine. The example uses
the curl installed on the system and does not compile it from source.

# Quick Start

To run the regression test, execute `make check`. For SGX, execute `make SGX=1 check`. The test
downloads the index page of `example.com` and therefore needs an Internet connection.

# Running under each backend

Build and run in this directory, with the install of the backend active (`Documentation/quickstart/`). Pass or fail status: matrix in [`CI-Examples/README.md`](../README.md).

- **direct:** `make`, then `make check` (needs `python3` for the test server).
- **vm:** `make check GRAMINE=gramine-vm`. Not verified.
- **sev:** `make check GRAMINE=gramine-sev`. Not verified.
- **vbs:** `make VBS=1 check` (builds, then runs).
