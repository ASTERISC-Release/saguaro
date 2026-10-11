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
void __attribute__((constructor)) sharedgl_entry(void) {
    fprintf(stderr, "[.] Bypass EGL Shared Library\n");

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
        /* Setup syscall communication regions */
        in_gramine_vm = true;
        fprintf(stderr, "[.] RUNTIME: Gramine VM\n");
        setup_syscall_comms();
    } else {
        fprintf(stderr, "[.] RUNTIME: Process\n");
    }


    fprintf(stderr, "========GBM-DEVICE========\n");
    __sync_synchronize();
    fprintf(stderr, "[.] DRM node: %s\n", drm_node);

    /* Open DRM render node and create GBM device */
    int drm_fd = open(drm_node, O_RDWR | O_CLOEXEC);
    if (drm_fd < 0) {
        perror("open(drm)");
        return;
    }
    fprintf(stderr, "[*] Node open success\n");

    gbm = gbm_create_device(drm_fd);
    if (!gbm) {
        fprintf(stderr, "gbm_create_device failed\n");
        close(drm_fd);
        return;
    }

    if (!in_gramine_vm) {
        create_and_setup_xcb_window();
        fprintf(stderr, "[*] XCB connection established\n");
    } else {
        fprintf(stderr, "[*] Skipping XCB connection setup in Gramine VM\n");
    }

    cur = 0;
    prev_present_fence = XCB_NONE;
    fprintf(stderr, "========GBM-DEVICE========\n");
}
