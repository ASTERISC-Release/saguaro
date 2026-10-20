<div class="program">

gramine

</div>

# `gramine-direct`, `gramine-sgx` -- Run an application with Gramine

> [!NOTE]
> This page is a stub.

## Synopsis

`gramine-direct` {\<\*APPLICATION\*\>} \[\<\*ARGS\*\> ...\]\
`gramine-sgx` {\<\*APPLICATION\*\>} \[\<\*ARGS\*\> ...\]

## Description

This is the main way to invoke Gramine. The first argument is the name of the
application (that is, name of the manifest file *without* `.manifest`).

## Options

<div class="option">

`--version`  

Show version and exit.

</div>

## Environment variables

<div class="envvar">

GRAMINE_NO_AUTO_GET_TOKEN

If not empty, for out-of-tree EPID driver `gramine-sgx` will not
automatically generate EINITTOKEN.

On upstream/DCAP driver the token is never generated and this variable has no
effect.

</div>
