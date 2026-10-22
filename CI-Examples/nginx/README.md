# Nginx

Runs the Nginx 1.22.0 web server (built from source by the Makefile) on `127.0.0.1:8002` (HTTP) and `8444` (HTTPS, self-signed certificate).

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
make start-gramine-server       # VM and SEV: add GRAMINE=gramine-vm (or gramine-sev) SAGUARO_VM_INBOUND=8002:8002,8444:8444
curl -s http://127.0.0.1:8002/random/10K.1.html | wc -c      # 10240
curl -sk https://127.0.0.1:8444/random/10K.1.html | wc -c    # HTTPS
```

`make check` starts the server, fetches pages and stops it (`make SGX=1`, `make VBS=1` for those backends).

## Notes

- Under VBS only HTTP on port 8002 is served (no SSL).
