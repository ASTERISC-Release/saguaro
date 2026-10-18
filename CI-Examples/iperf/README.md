# iperf

This directory contains the Makefile and the template manifest for the most
recent version of `iperf` as of this writing (3.16).

# Prerequisites

`iperf` has no prerequisites
([source](https://github.com/esnet/iperf/tree/3.16?tab=readme-ov-file#prerequisites)).

# Building instructions

`iperf` is built from source because the Ubuntu 22.04 package `iperf3` v3.9 requires
`TCP_CONGESTION` (it is built with the `iperf3_cv_header_tcp_congestion="yes"` configuration
option). Using the Ubuntu package fails with the following error:
```
iperf3: error - unable to set TCP_CONGESTION: ...
```

The manual build omits `TCP_CONGESTION` and runs under Gramine[^1].

[^1]: Starting from version 3.10, `iperf` supports environments that do not
implement congestion control algorithm. Thus, iperf 3.10+ prebuilt packages
should work under Gramine without problems. See [release
notes](https://github.com/esnet/iperf/blob/3.16/RELNOTES.md#iperf-310-2021-05-26)
for details.


## Building for Linux

Run `make` in the current directory.

## Building for SGX

Run `make SGX=1` (non-debug) or `make SGX=1 DEBUG=1` (debug) in the current
directory.

# Execution instructions

Run `iperf` server natively:
```
LD_LIBRARY_PATH=./install ./install/iperf3 -s
```

Run `iperf` server in Gramine without SGX:
```
gramine-direct iperf3
```

Run `iperf` server in Gramine with SGX:
```
gramine-sgx iperf3
```

To get measurements with the `iperf` client, run in another terminal:
```
LD_LIBRARY_PATH=./install ./install/iperf3 -c localhost -p 5201
```

# Useful iperf options

(The options may be version-dependent. Below are the options for v3.16.)

## Generic options (both for server and client):
- `-p, --port`: server port to listen on/connect to
- `--forceflush`: force flushing output at every interval
- `-d, --debug[=#]`: emit debugging output (optional "=" and debug level: 1-4)

## Server-specific options:
- `-s, --server`: run in server mode
- `-1, --one-off`: handle one client connection then exit
- `--idle-timeout #`: restart idle server after # seconds in case it got stuck

## Client-specific options:
- `-c, --client <host>`: run in client mode, connecting to `<host>`
- `-t, --time #`: time in seconds to transmit for (default 10 secs)
- `-n, --bytes #[KMG]`: number of bytes to transmit (instead of -t)
- `-P, --parallel #`: number of parallel client streams to run
- `-N, --no-delay`: set TCP/SCTP no delay, disabling Nagle's Algorithm

# Notes
- Tested on Ubuntu 22.04.
- The execution instructions use port `5201` for the client. This is the default port of `iperf`.

# Running under each backend

Build first (`make`, or `make VBS=1` for vbs) and activate the install of the backend (`Documentation/quickstart/`). Results and dates are in the matrix of `CI-Examples/README.md`.

- **direct:** not verified.¹
- **vm:** not verified.¹
- **sev:** not verified.¹
- **tdx:** not verified.²
- **sgx:** not verified.³ The commands are given above.
- **vbs:** `make VBS=1 check`. Verified on ARM64 and x64.

¹ No result is recorded for this backend (matrix in `CI-Examples/README.md`).  
² TDX was tested offline by the maintainer, with the same result as SEV. No TDX log is in the repository, so the result is reported and not verified here. The TDX install needs `td-shim` (`Documentation/TODO.md`).  
³ SGX has not run on this tree's hardware. The SGX build needs `protobuf-c` (`Documentation/TODO.md`).
