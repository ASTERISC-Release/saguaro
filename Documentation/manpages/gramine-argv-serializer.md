<div id="gramine-argv-serializer">

<div class="program">

gramine-argv-serializer

</div>

</div>

# `gramine-argv-serializer` -- Serialize command line arguments

## Synopsis

`gramine-argv-serializer` \[*ARGS*\]

## Description

<span class="title-ref">gramine-argv-serializer</span> serializes the command line arguments and prints it
to stdout. The result is intended to be redirected to a file that can
be used as a trusted or a protected file and `loader.argv_src_file` manifest
option pointed to it.
For more information on the usage, please refer to `../manifest-syntax`.

For an example on how to use this utility from Python, please refer to
`libos/test/regression/test_libos.py`.

## Example

``` sh
gramine-argv-serializer "binary_name" "arg1" "arg2" > gramine_args.txt
```
