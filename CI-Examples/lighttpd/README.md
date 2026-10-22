# Lighttpd

Runs the lighttpd 1.4.59 web server (built from source by the Makefile) on `127.0.0.1:8003`.

## Prerequisites

Packages `build-essential`, `libssl-dev` and `zlib1g-dev`.

## Run

Install the backend first: [`Documentation/quickstart/`](../../Documentation/quickstart/README.md).

```sh
./exec.sh --direct      # builds, runs and checks; also: --vm, --sev, --sev-snp, --sgx, --vbs
```

By hand, the server in one terminal and the client in another:

```sh
make
make start-saguaro-server       # VM and SEV: add SAGUARO=saguaro-vm (or saguaro-sev) SAGUARO_VM_INBOUND=8003:8003
curl -s http://127.0.0.1:8003/random/10K.1.html | wc -c      # 10240
```

`make check` starts the server, fetches pages and stops it (`make SGX=1`, `make VBS=1` for those backends).

## Notes

- Under VBS the build has no zlib, OpenSSL, PCRE or bzip2, so no `mod_deflate`.
