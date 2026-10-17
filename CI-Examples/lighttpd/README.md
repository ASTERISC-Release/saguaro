# Lighttpd Example

This directory contains an example for running lighttpd in Gramine, including
the Makefile and a template for generating the manifest.

# Building lighttpd source

For this example, we build lighttpd from source instead of using an existing
binary. To build lighttpd on Ubuntu 20.04, please make sure that the following
packages are installed:

    sudo apt-get install -y build-essential libssl-dev zlib1g-dev

## Linux

Run `make` (non-debug) or `make DEBUG=1` (debug) in the directory.

## SGX

Run `make SGX=1` (non-debug) or `make SGX=1 DEBUG=1` (debug) in the directory to
prepare lighttpd to run on SGX.

# Running lighttpd

Execute one of the following commands to start lighttpd either natively
(non-Gramine), on Gramine or Gramine-SGX, respectively.

    make start-native-server
    make start-gramine-server
    SGX=1 make start-gramine-server

Because these commands will start the lighttpd server in the foreground, you
will need to open another console to run the client.

Once the server has started, you can test it with `wget` or `curl`

    wget http://127.0.0.1:8003/random/10K.1.html
    curl --compressed http://127.0.0.1:8003/random/10K.1.html -o 10K.1.html

You may also run the benchmark script using `wrk` (wrk2). Please refer to
https://github.com/giltene/wrk2 for more information.

    ../common_tools/benchmark-http.sh http://127.0.0.1:8003

Use Ctrl-C to terminate the server once you are finished testing lighttpd.

# lighttpd in a Windows VBS enclave

In WSL on an ARM64 Windows machine with VBS (`Documentation/vbs.md`, "Makefile flow"):

```sh
make VBS=1          # lighttpd built with the vbs gcc, installed into vbs-build/root, packed, signed
make VBS=1 check    # start it in an enclave; fetch pages with Windows' curl.exe (vbs-check.sh)
```

lighttpd runs in the foreground (`-D`) as one process on 127.0.0.1:8003 with
`lighttpd-vbs.conf.template` (no mod_deflate: the vbs build has no zlib, OpenSSL, PCRE or bzip2).
Its modules are loaded with `dlopen` from `/lighttpd/lib`; they are packed into the signed image
like the binary.
