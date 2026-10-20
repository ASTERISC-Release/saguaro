<div class="program">

gramine-sgx-sign

</div>

# `gramine-sgx-sign` -- Gramine `SIGSTRUCT` generator

> [!NOTE]
> This page is inherited from upstream Gramine and describes the Intel SGX backend. This tree
> still builds that backend but does not test it; see `README.md` for what is validated here.

## Synopsis

`gramine-sgx-sign` \[*OPTION*\]... --output output_manifest
--key key_file --manifest manifest_file

## Description

`gramine-sgx-sign` is used to expand Trusted Files and generate
signature file for given input manifest and libpal file (main Gramine binary).

## Command line arguments

<div class="option">

`--help, -h`  

Show help and exit.

</div>

<div class="option">

`--output output_manifest, -o output_manifest`  

Path to the output manifest file (with Trusted Files expanded).

</div>

<div class="option">

`--key key_file, -k key_file`  

Path to the private key used for signing.

</div>

<div class="option">

`--manifest manifest_file, -m manifest_file`  

Input manifest file.

</div>

<div class="option">

--date \<YYYY-MM-DD\>\|today

Set specific date to be put into `SIGSTRUCT`. If not given, or the value
is literal `today`, then current day according to system calendar is used.
Otherwise expects `<YYYY>-<MM>-<DD>`. The date needs not to be a \|~\| valid
day, it will happily accept `--date 0000-00-00`, e.g. for reproducible
builds.

</div>

<div class="option">

`--libpal libpal_path, -l libpal_path`  

Path to libpal file (main Gramine binary).

</div>

<div class="option">

`--sigfile sigfile, -s sigfile`  

Path to the output file containing `SIGSTRUCT`. If not provided,
<span class="title-ref">manifest_file</span> will be used with ".manifest" (if present) removed from
the end and with ".sig" appended.

</div>

<div class="option">

`--depfile depfile`  

Generate a file that describes the dependencies for the output manifest and
`SIGSTRUCT`, i.e. files that should trigger rebuilding if they're
modified. The dependency file is in Makefile format, and is suitable for
using in build systems (Make, Ninja).

</div>

<div class="option">

`--chroot <path>`  

When calculating cryptographic hashes of trusted files, measure files inside
a \|~\| chroot instead of paths in root of the file system. Requires that all
paths in manifest are absolute, and those will be interpreted as relative to
the directory specified as the value of the option.

Note you need to be very careful that the Gramine runtime binaries are
exactly the same inside chroot as the ones used to execute
`gramine-sgx-sign`.

</div>

<div class="option">

`--verbose, -v`  

Print details to standard output. This is the default.

</div>

<div class="option">

`--quiet, -q`  

Don't print details to standard output.

</div>

<div class="option">

`--with <plugin>`  

Use plugin to perform actual signing. The default plugin is `file`, which
signs the `SIGSTRUCT` using PEM-encoded local file. The list of available
plugins is at the end of `--help` output.

Each plugin may add its own set of options (usually in the form of
`--<plugin>-<option>`). To get help about those, use
`gramine-sgx-sign --with=<plugin> --help-<plugin>` and/or consult
the documentation of the respective plugin.

</div>
