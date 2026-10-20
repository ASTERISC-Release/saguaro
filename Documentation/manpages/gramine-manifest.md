<div class="program">

gramine-manifest

</div>

# `gramine-manifest` -- Gramine manifest preprocessor

## Synopsis

`gramine-manifest` \[*OPTION*\]... \[*SOURCE-FILE* \[*OUTPUT-FILE*\]\]

## Description

`gramine-manifest` is used to preprocess manifests for Gramine using
[Jinja markup](https://jinja.palletsprojects.com/).

## Command line arguments

<div class="option">

--define \<key\>=\<value\>, -D \<key\>=\<value\>

Have a \|~\| variable available in the template.

</div>

<div class="option">

`--check`  

After rendering manifest from template, perform validation against manifest
schema to check for unknown manifest entries and/or missing mandatory
options. See `gramine-manifest-check` for more details.

The check is enabled by default. This option serves to re-enable the check
after `--no-check`.

For the 1.7 release, only a \|~\| warning is issued and
`gramine-manifest` proceeds to write the faulty manifest. In version
1.8 this will be a \|~\| hard error.

</div>

<div class="option">

`--no-check`  

Disable schema validation, as described above in `--check`.

</div>

## Functions and constants available in templates

<div class="default-domain">

py

</div>

<div class="data">

gramine.libos

Path to `libsysdb.so`.

</div>

<div class="function">

gramine.runtimedir(libc='glibc')

The path to runtime directory with patched libc. Currently supported options
are: `'glibc'` and `'musl'`.

</div>

<div class="data">

python.stdlib

`stdlib` installation path from [sysconfig module](https://docs.python.org/library/3/sysconfig.html#installation-paths)

</div>

<div class="data">

python.platstdlib

`platstdlib` installation path from [sysconfig module](https://docs.python.org/library/3/sysconfig.html#installation-paths)

</div>

<div class="data">

python.purelib

`purelib` installation path from [sysconfig module](https://docs.python.org/library/3/sysconfig.html#installation-paths)

</div>

<div class="data">

python.distlib

On Debian systems, this is `/usr/lib/python3/dist-packages`.

</div>

<div class="function">

python.get_path(...)

[sysconfig.get_path](https://docs.python.org/3/library/sysconfig.html#sysconfig.get_path)

</div>

<div class="function">

python.get_paths(...)

[sysconfig.get_paths](https://docs.python.org/3/library/sysconfig.html#sysconfig.get_paths)

</div>

<div class="data">

python.implementation

[sys.implementation](https://docs.python.org/3/library/sys.html#sys.implementation)

</div>

<div class="data">

env.\[ENVVAR\]

The content of `$ENVVAR` environment variable.

</div>

<div class="function">

ldd(\*executables)

List of libraries which are linked from *executables*. Each library is
provided at most once.

</div>

## Example

`manifest.template`:

``` jinja
libos.entrypoint = "{{ entrypoint }}"
loader.env.LD_LIBRARY_PATH = "/lib:{{ arch_libdir }}:/usr{{ arch_libdir }}"

fs.mounts = [
  { path = "/lib", uri = "file:{{ gramine.runtimedir() }}" },
  { path = "/{{ entrypoint }}", uri = "file:{{ entrypoint }}" },
]

sgx.trusted_files = [
  "file:{{ entrypoint }}",
  "file:{{ gramine.runtimedir() }}/",
]
```

`Makefile`:

```
%.manifest: manifest.template
   gramine-manifest \
      -Dentrypoint=$(ENTRYPOINT) \
      -Darch_libdir=$(ARCH_LIBDIR) \
      $< $@
```
