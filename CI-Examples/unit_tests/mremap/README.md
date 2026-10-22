# mremap

Tests `mremap(2)` without `MREMAP_MAYMOVE`: a mapping is grown in place and shrunk in place; the address must not change and the data must survive.

```sh
make
saguaro-direct helloworld        # prints "ALL IN-PLACE TESTS PASSED"
```

`../exec.sh --direct` (or `--vm`, `--sev`, `--sev-snp`) builds, runs and checks it. `make check` here runs it and requires `ALL IN-PLACE TESTS PASSED`.
