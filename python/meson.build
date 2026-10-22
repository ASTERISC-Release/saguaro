subdir('graminelibos')
subdir('graminelibos.dist-info')

install_data([
    'saguaro-gen-depend',
    'saguaro-manifest',
    'saguaro-manifest-check',
], install_dir: get_option('bindir'))

if enable_tests
    install_data([
        'saguaro-test',
    ], install_dir: get_option('bindir'))
endif

if sgx
    install_data([
        'saguaro-sgx-gen-private-key',
        'saguaro-sgx-get-token',
        'saguaro-sgx-sign',
        'saguaro-sgx-sigstruct-view',
    ], install_dir: get_option('bindir'))
endif
