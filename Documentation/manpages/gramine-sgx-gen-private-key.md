<div class="program">

gramine-sgx-gen-private-key

</div>

# `gramine-sgx-gen-private-key` -- Gramine SGX key generator

> [!NOTE]
> This page is inherited from upstream Gramine and describes the Intel SGX backend. This tree
> still builds that backend but does not test it; see `README.md` and
> `Documentation/devel/multiarch.md` for what is validated here.

## Synopsis

`gramine-sgx-gen-private-key` \[*OPTION*\]... \[*FILE*\]

## Description

`gramine-sgx-gen-private-key` is used to generate the private key
suitable for signing SGX enclaves. SGX requires RSA 3072 keys with public
exponent equal to 3.

The default path is `$XDG_CONFIG_HOME/gramine/enclave-key.pem` (which is
usually `$HOME/.config/gramine/enclave-key.pem` — see [XDG Base Directory
Specification](https://specifications.freedesktop.org/basedir-spec/basedir-spec-latest.html)
for details). You can specify an alternative path as an argument.

## Command line arguments

<div class="option">

`--force, -f`  

By default `gramine-sgx-gen-private-key` will refuse to overwrite an
existing file. Use this switch to force clobbering.

</div>

<div class="option">

`--no-force`  

Refuse to overwrite an existing file, which is the default. Use this switch
to undo `--force`.

</div>

<div class="option">

`--verbose, -v`  

Print details to standard output. This is the default.

</div>

<div class="option">

`--quiet, -q`  

Don't print details to standard output.

</div>
