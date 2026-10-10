subdir('include')
subdir('src')

# the PAL regression tests are Linux processes linked against libpal, which only the direct/SGX
# PALs provide as a library (the VM PAL is a bare-metal kernel image)
if enable_tests and (direct or sgx)
    subdir('regression')
endif

if debug
    install_data(
        'gdb_integration/gramine.gdb',
        'gdb_integration/debug_map_gdb.py',
        'gdb_integration/language_gdb.py',
        'gdb_integration/pagination_gdb.py',
        install_dir: pkglibdir / 'gdb_integration',
    )
endif
