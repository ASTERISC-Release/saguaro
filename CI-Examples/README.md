# Example Integrations

This folder contains application integration examples. They are tested on
Ubuntu 24.04 (aarch64 and x86_64) with `gramine-direct`.

For instructions how to build and run each application under Gramine, please
see the README.md in each subdirectory.

Please note that most of the examples use oversimplified configurations which
are not secure (e.g., they mount whole host directories into the application).
If you take these examples as templates for your own workloads, please inspect
and harden the configurations.

## Prerequisites

Install and enable Gramine first (`scripts/dependencies.sh`, `scripts/install.sh`
and `. ~/.local/gramine/env.sh`, see the top-level README). Some examples need
extra host packages: each has an `install-prereqs.sh`, and
`install-all-prereqs.sh` runs all of them.

## Building Examples

All examples use simple Makefiles to build the application and generate its
Gramine manifest. Use one of these commands:
- `make`: create a no-debug-log manifest
- `make DEBUG=1`: create a debug-log manifest

Then run the application with `gramine-direct <application>`.

Use `make clean` to remove Gramine-generated artifacts and `make distclean` to
remove all build artifacts (if applicable).

## Testing

`scripts/test.sh` (in the top-level `scripts/` directory) builds and runs most
of the examples as part of the test battery; `scripts/test.sh --list` shows the
suites. `unit_tests/` holds small test programs for LibOS and PAL behavior.
