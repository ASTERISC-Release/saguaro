# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The ASTERISC Research Lab

# libgraphics: the library preloaded (LD_PRELOAD) into a graphics application that runs under a
# VM-type PAL with the Tirith graphics redirection. It replaces the application's GLX setup with
# EGL on GBM and asks the host listener to present; see Documentation/devel/graphics/.
#
# Unlike the rest of this directory it is an ordinary hosted library: it links against the host's
# libc, Mesa and X libraries and is not part of libsysdb.so. It is built only where it can be used
# (see the condition in ../meson.build) and its dependencies are then required.

graphics_deps = []
foreach name : ['egl', 'gl', 'gbm', 'libdrm', 'x11', 'xcb', 'xcb-dri3', 'xcb-present', 'xcb-sync',
                'xshmfence', 'sdl2']
    dep = dependency(name, required: false)
    if not dep.found()
        error('-Dgraphics=enabled with a VM-type PAL builds the graphics preload library (libgraphics.so), ' +
              'which needs the development package for "@0@" (pkg-config). '.format(name) +
              'Install it, or configure with -Dgraphics=disabled.')
    endif
    # SDL2 is needed for its headers only (the library interposes a few SDL entry points).
    if name == 'sdl2'
        dep = dep.partial_dependency(compile_args: true, includes: true)
    endif
    graphics_deps += dep
endforeach

libgraphics = shared_library('graphics',
    'egl.c',
    'glx.c',
    'gramine.c',
    'log.c',
    'main.c',
    'xcb.c',

    # The sources predate this tree's warning set and use GNU C; cleaning them up is a separate
    # task.
    override_options: ['c_std=gnu11', 'werror=false'],
    c_args: [
        # log.c interposes both open() and open64() (and the mmap/creat/openat pairs); with meson's
        # default _FILE_OFFSET_BITS=64 the plain names become aliases of the 64-bit ones.
        '-U_FILE_OFFSET_BITS',

        '-Wno-missing-prototypes',
        '-Wno-unused-function',
        '-Wno-unused-variable',
        '-Wno-strict-prototypes',
        '-Wno-unused-parameter',
        '-Wno-sign-compare',
        cc.get_supported_arguments(
            '-Wno-error=implicit-function-declaration',
            '-Wno-error=return-mismatch',
        ),
    ],
    dependencies: [
        graphics_deps,
        cc.find_library('dl', required: false),
        cc.find_library('m'),
    ],
    install: true,
    install_dir: pkglibdir,
)
