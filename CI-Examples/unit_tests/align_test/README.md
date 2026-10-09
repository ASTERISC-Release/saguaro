# align_test

An allocation and alignment probe: it allocates a number of blocks, prints a pointer and then
waits. It is meant for manual inspection, not as a pass/fail test, so its Makefile has no working
`check` target.

    make
    gramine-direct align_test
