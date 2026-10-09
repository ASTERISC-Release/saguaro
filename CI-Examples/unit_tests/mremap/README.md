# mremap

Tests `mremap(2)` without `MREMAP_MAYMOVE`: a mapping is grown in place, filled and checked, then
shrunk in place and checked again. The address must not change and the data must survive.

    make
    gramine-direct helloworld        # prints "ALL IN-PLACE TESTS PASSED"

(`make check` is not wired up for this example.) See `libos/src/sys/libos_mremap.c`.
