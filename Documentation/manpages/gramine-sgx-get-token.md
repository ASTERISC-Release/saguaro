<div class="program">

gramine-sgx-get-token

</div>

# `gramine-sgx-get-token` -- Gramine SGX token generator

> [!NOTE]
> This page is inherited from upstream Gramine and describes the Intel SGX backend. This tree
> still builds that backend but does not test it; see `README.md` and
> `Documentation/devel/multiarch.md` for what is validated here.

## Synopsis

`gramine-sgx-get-token` \[*OPTION*\]... --sig sigstruct_file
--output token_file

## Description

`gramine-sgx-get-token` is used to generate the SGX token file for
given SIGSTRUCT (".sig" file).

Using this command is not necessary (it was previously), since the token is
fetched automatically if needed during the first enclave start.

On upstream/DCAP driver this command does nothing and is deprecated. In
the future, it will be removed altogether.

## Command line arguments

<div class="option">

`--sig sigstruct_file, -s sigstruct_file`  

Path to the input file containing SIGSTRUCT.

</div>

<div class="option">

`--output token_file, -o token_file`  

Path to the output token file.

</div>

<div class="option">

`--verbose, -v`  

Print details to standard output. This is the default.

</div>

<div class="option">

`--quiet, -q`  

Don't print details to standard output.

</div>
