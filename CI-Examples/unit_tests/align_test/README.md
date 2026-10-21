# align_test

An allocation and alignment probe: it allocates blocks, prints a pointer and waits. It is for manual inspection, not a pass/fail test (`make check` compares with `Hello, world` and fails).

```sh
make
gramine-direct align_test
```
