/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

#define _GNU_SOURCE
#include <GL/gl.h>
#include <GL/glx.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <assert.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <sys/syscall.h>
#include <dirent.h>
#include <time.h>
#include <unistd.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <gbm.h>
#include <xcb/dri3.h>
#include <xcb/present.h>
#include <xcb/sync.h>
#include <xcb/xcb.h>

#include <GL/gl.h>
#include <GL/glext.h>
#include <math.h>
#include <xcb/dri3.h>
#include <xcb/present.h>
#include <xcb/xfixes.h>

#include <X11/xshmfence.h>
#include <drm/drm.h>
#include <drm/drm_fourcc.h>
#include <drm/i915_drm.h>
#include <gbm.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include "common.h"
#include <X11/xshmfence.h>

/* TODO: fix the naming scheme */
int cur = 0;
xcb_window_t win;
xcb_connection_t *conn;
xcb_sync_fence_t prev_present_fence = XCB_NONE;
bool in_gramine_vm = false;
bool dump_png = false;
char *drm_node = "/dev/dri/renderD128";
static int64_t boot_t0_ms = -1;
/* Guest-side boot instrumentation: ms since the preload-library constructor ran. */
static long boot_elapsed_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    int64_t now = (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
    if (boot_t0_ms < 0) {
        boot_t0_ms = now;
        return 0;
    }
    return (long)(now - boot_t0_ms);
}

static bool open_gbm_device(void) {
    fprintf(stderr, "========GBM-DEVICE========\n");
    __sync_synchronize();
    fprintf(stderr, "[.] DRM node: %s\n", drm_node);

    /* Open DRM render node and create GBM device */
    int drm_fd = open(drm_node, O_RDWR | O_CLOEXEC);
    if (drm_fd < 0) {
        perror("open(drm)");
        return false;
    }
    fprintf(stderr, "[*] Node open success\n");

    gbm = gbm_create_device(drm_fd);
    if (!gbm) {
        fprintf(stderr, "gbm_create_device failed\n");
        close(drm_fd);
        return false;
    }
    fprintf(stderr, "[boot] GPU/GBM ready +%ld ms\n", boot_elapsed_ms());

    if (!in_gramine_vm) {
        create_and_setup_xcb_window();
        fprintf(stderr, "[*] XCB connection established\n");
    } else {
        fprintf(stderr, "[*] Skipping XCB connection setup in Gramine VM\n");
    }

    cur = 0;
    prev_present_fence = XCB_NONE;
    fprintf(stderr, "========GBM-DEVICE========\n");
    return true;
}

void __attribute__((constructor)) sharedgl_entry(void) {
    fprintf(stderr, "[.] Bypass EGL Shared Library\n");
    boot_elapsed_ms(); /* arms the [boot] clock at the library constructor */

    /* Check environment variables for logging */
    const char *ioctl_log = getenv("EGL_IOCTL_LOG");
    if (ioctl_log && strcmp(ioctl_log, "1") == 0) {
        fprintf(stderr, "[.] LOG: IOCTL logging enabled\n");
        ioctl_logging_enabled = true;
    }
    const char *syscall_log = getenv("EGL_SYSCALL_LOG");
    if (syscall_log && strcmp(syscall_log, "1") == 0) {
        fprintf(stderr, "[.] LOG: Syscall logging enabled\n");
        set_syscall_logging(1);
    }
    const char* png_log = getenv("EGL_DUMP_PNG");
    if (png_log && strcmp(png_log, "1") == 0) {
        dump_png = true;
        fprintf(stderr, "[.] LOG: Dumping frames as png images\n");
    } 

    /* Check and use different GPUs */
    const char *egl_discrete = getenv("EGL_DISCRETE");
    if (egl_discrete && strcmp(egl_discrete, "1") == 0) {
        drm_node = "/dev/dri/renderD129";
        fprintf(stderr, "[.] GPU: Discrete (%s)\n", drm_node);
    } else {
        fprintf(stderr, "[.] GPU: Integrated (%s)\n", drm_node);
    }

    /* Check the gramine environment variable */
    const char *egl_gramine = getenv("EGL_GRAMINE");
    if (egl_gramine && strcmp(egl_gramine, "1") == 0) {
        /* Under the VM the graphics session (the comm-page handshake with the host listener and
         * the GBM device on the forwarded render node) starts at the first graphics call, in
         * graphics_ensure_session(), not here: a process that inherits LD_PRELOAD but never draws
         * (for example a `sh -c` child of a game) must not run the handshake, which would wait
         * for a listener that is serving the parent and reset the comm page under it. */
        in_gramine_vm = true;
        /* EGL_EAGER_SESSION=1: start the session here, for programs that make no GLX, EGL or
         * Vulkan instance call through this library first (Vulkan programs that load the Vulkan
         * loader with dlopen(), see vulkan.c). The comm-page handshake needs this library's
         * constructor to run once per process that draws, so only set it for such programs. */
        const char *eager = getenv("EGL_EAGER_SESSION");
        if (eager && strcmp(eager, "1") == 0) {
            fprintf(stderr, "[.] RUNTIME: Gramine VM (EGL_EAGER_SESSION: graphics session starts now)\n");
            graphics_ensure_session();
            return;
        }
        fprintf(stderr, "[.] RUNTIME: Gramine VM (graphics session starts at the first graphics call)\n");
        return;
    }
    fprintf(stderr, "[.] RUNTIME: Process\n");
    open_gbm_device();
}

static bool vm_session_ready = false;

static void start_vm_session(void) {
    fprintf(stderr, "[.] graphics session: starting\n");
    /* Setup syscall communication regions */
    setup_syscall_comms();
    fprintf(stderr, "[boot] guest comms ready +%ld ms\n", boot_elapsed_ms());
    vm_session_ready = open_gbm_device();
}

/* Under the VM, starts the graphics session once (any thread); returns whether it is usable. Outside
 * the VM the constructor has already done everything, and this returns true. */
bool graphics_ensure_session(void) {
    if (!in_gramine_vm)
        return true;
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, start_vm_session);
    return vm_session_ready;
}
