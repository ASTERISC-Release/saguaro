# Nginx

This directory contains the Makefile and the template manifest for the most
recent version of Nginx web server (as of this writing, version 1.16.1).

We build Nginx from the source code instead of using an existing installation.
On Ubuntu 20.04, please make sure that the following packages are installed:
```sh
sudo apt-get install -y build-essential libssl-dev zlib1g-dev
```

NOTE: The "benchmark-http.sh" script uses the wrk benchmark (wrk2) under the
hood. Please refer to https://github.com/giltene/wrk2.

# Quick Start

```sh
# build Nginx and the final manifest
make SGX=1

# run original Nginx against HTTP and HTTPS benchmarks (benchmark-http.sh, uses wrk2)
./install/sbin/nginx -c conf/nginx-gramine.conf &
../common_tools/benchmark-http.sh http://127.0.0.1:8002
../common_tools/benchmark-http.sh https://127.0.0.1:8444
kill -SIGINT %%

# Run Nginx in non-SGX Gramine against HTTP and HTTPS benchmarks.
# Note: The command-line arguments are passed using `loader.argv_src_file`
# manifest option.
gramine-direct ./nginx &
../common_tools/benchmark-http.sh http://127.0.0.1:8002
../common_tools/benchmark-http.sh https://127.0.0.1:8444
kill -SIGINT %%

# Run Nginx in Gramine-SGX against HTTP and HTTPS benchmarks.
# Note: The command-line arguments are securely passed using
# `loader.argv_src_file` manifest option.
gramine-sgx ./nginx &
../common_tools/benchmark-http.sh http://127.0.0.1:8002
../common_tools/benchmark-http.sh https://127.0.0.1:8444
kill -SIGINT %%

# you can also test the server using other utilities like wget
wget http://127.0.0.1:8002/random/10K.1.html
wget https://127.0.0.1:8444/random/10K.1.html
```

Alternatively, to run the Nginx server, use one of the following commands:

```
make start-native-server
make start-gramine-server
make SGX=1 start-gramine-server
```

# NGINX in a Windows VBS enclave

In WSL on an ARM64 Windows machine with VBS (`Documentation/vbs.md`, "Makefile flow"):

```sh
make VBS=1          # NGINX built with the vbs gcc, installed into vbs-build/root, packed, signed
make VBS=1 check    # start it in an enclave; fetch pages with Windows' curl.exe (vbs-check.sh)
../../scripts/vbs/gramine-vbs nginx &
../../scripts/vbs/gramine-vbs win curl.exe -s http://127.0.0.1:8002/random/10K.1.html | wc -c
../../scripts/vbs/gramine-vbs net bench http 8002 -c 8 -n 40000 -p /random/2K.1.html
kill %%
```

The configuration (`nginx-vbs.conf.template`) runs one process: `master_process off` (the
master would fork the workers), `worker_processes 1`, `daemon off`; HTTP only on 127.0.0.1:8002
(the vbs sysroot has no OpenSSL, zlib or PCRE: no SSL, gzip, rewrite or basic authentication
modules). NGINX's prefix is `/nginx` in the enclave, everything below it (binary, configuration,
`html/` with the test files) is packed into the signed image and read-only; the pid file and
temporary files go to `/tmp`, an in-enclave tmpfs, and the error log to stderr.

# Running under each backend

Run the commands in this directory after the build (`make`, or `make VBS=1`), with the install of the backend active (`Documentation/quickstart/`). The matrix in `CI-Examples/README.md` gives the result and the date for each backend. "Not verified" means that no result is recorded.

- **direct:** `gramine-direct ./nginx`. Verified on x86_64 and aarch64.
- **vm:** `GRAMINE_RUNNER=gramine-vm test/test-direct.sh nginx`, from the repository root (the VM forwards the server's port through its relay; nginx runs with `master_process off`). Verified on x86_64 and aarch64.
- **sev:** not verified.¹
- **tdx:** not verified.²
- **sgx:** not verified.³ Upstream command: `gramine-sgx ./nginx`.
- **vbs:** `make VBS=1 check`. Verified on ARM64 and x64.

¹ Not verified: no result is recorded for this backend (matrix in `CI-Examples/README.md`).  
² TDX was tested offline by the maintainer, with the same result as SEV. No TDX log is in the repository, so the result is reported and not verified here. The TDX install needs `td-shim` (`Documentation/TODO.md`).  
³ SGX has not run on this tree's hardware. The SGX build needs `protobuf-c` (`Documentation/TODO.md`).
