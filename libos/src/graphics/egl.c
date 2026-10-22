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
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
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
#include <drm/drm_fourcc.h>
#include <gbm.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include "common.h"
#include "graphics_comm_abi.h"
#include <sys/ioctl.h>
#include <X11/xshmfence.h>

#define syscall_register_host_identity_fd 451

/* Mesa's internal sRGB alias for DRM_FORMAT_XRGB8888. It describes the GL interpretation of the
 * shared storage; the GBM BO and the host X11 pixmap remain ordinary XRGB8888. */
#define MESA_DRI_IMAGE_FOURCC_SXRGB8888 0x85324258

struct gbm_device *gbm;
static check bufs_static[NUM_BUFFERS];
check* bufs = bufs_static;
uint64_t bufs_pa;

PFNGLFRAMEBUFFERTEXTURE2DPROC glFramebufferTexture2D_ptr;
PFNGLFENCESYNCPROC glFenceSync_ptr = NULL;
PFNGLDELETESYNCPROC glDeleteSync_ptr = NULL;
PFNGLCLIENTWAITSYNCPROC glClientWaitSync_ptr = NULL;

PFNGLGENFRAMEBUFFERSPROC glGenFramebuffers_ptr = NULL;
PFNEGLGETPLATFORMDISPLAYEXTPROC eglGetPlatformDisplayEXT;
PFNEGLCREATEIMAGEKHRPROC eglCreateImageKHR_ptr;
PFNEGLDESTROYIMAGEKHRPROC eglDestroyImageKHR_ptr;
PFNGLCHECKFRAMEBUFFERSTATUSPROC glCheckFramebufferStatus_ptr;
PFNGLFRAMEBUFFERRENDERBUFFERPROC glFramebufferRenderbuffer_ptr;
PFNGLGENRENDERBUFFERSPROC glGenRenderbuffers_ptr;
PFNGLBINDRENDERBUFFERPROC glBindRenderbuffer_ptr;
PFNGLRENDERBUFFERSTORAGEPROC glRenderbufferStorage_ptr;
PFNGLFRAMEBUFFERPARAMETERIPROC glFramebufferParameteri_ptr;

GLuint fbo;
EGLDisplay eglDpy;
static EGLContext eglCtx = EGL_NO_CONTEXT;
static EGLConfig eglCfg;
static pthread_mutex_t egl_setup_lock = PTHREAD_MUTEX_INITIALIZER;
static int egl_buffer_width;
static int egl_buffer_height;
static uint64_t egl_buffer_generation;
static _Thread_local EGLContext thread_egl_ctx = EGL_NO_CONTEXT;
static _Thread_local uint64_t thread_egl_generation;
static _Thread_local GLuint thread_fbos[NUM_BUFFERS];
PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glEGLImageTargetTexture2DOES_ptr;
PFNGLBINDFRAMEBUFFERPROC glBindFramebuffer_ptr;
int win_width = DEFAULT_WIDTH;
int win_height = DEFAULT_HEIGHT;
extern char *drm_node;

static void configure_redirect_orientation(void) {
    if (in_gramine_vm)
        return;

    /* X11 presents imported DMA-BUF rows from top to bottom. Associate the
     * inverse coordinate system with only our redirected FBO; changing global
     * glClipControl state corrupts applications that bind separate read and
     * draw framebuffers. */
    glFramebufferParameteri_ptr(GL_FRAMEBUFFER, GL_FRAMEBUFFER_FLIP_Y_MESA,
                                GL_TRUE);
}

static void setup_thread_fbos(void) {
    for (int i = 0; i < NUM_BUFFERS; ++i) {
        glGenFramebuffers_ptr(1, &thread_fbos[i]);
        glBindFramebuffer_ptr(GL_FRAMEBUFFER, thread_fbos[i]);
        configure_redirect_orientation();
        glFramebufferTexture2D_ptr(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                                   bufs[i].tex, 0);
        glFramebufferRenderbuffer_ptr(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                                      GL_RENDERBUFFER, bufs[i].rbo_depth);
        GLenum status = glCheckFramebufferStatus_ptr(GL_FRAMEBUFFER);
        if (status != GL_FRAMEBUFFER_COMPLETE) {
            fprintf(stderr, "FBO incomplete for buffer %d: 0x%x\n", i, status);
            assert(false);
        }
    }
    thread_egl_generation = egl_buffer_generation;
}

GLuint get_redirect_fbo(int buffer_index) {
    if (buffer_index < 0 || buffer_index >= NUM_BUFFERS ||
            thread_egl_generation != egl_buffer_generation) {
        return 0;
    }
    return thread_fbos[buffer_index];
}

int test_prime_to_handle(int dma_fd) {
    int drm_fd = open(drm_node, O_RDWR | O_CLOEXEC);
    if (drm_fd < 0) {
      perror("open(drm)");
      return -1;
    }

    struct drm_prime_handle p = { .fd = dma_fd, .flags = 0 };
    if (ioctl(drm_fd, DRM_IOCTL_PRIME_FD_TO_HANDLE, &p) < 0) {
        perror("PRIME_FD_TO_HANDLE");
        return -1;
    }
    fprintf(stderr, "PRIME_FD_TO_HANDLE OK, handle=%u\n", p.handle);
    return 0;
}

/* Mesa only imports a dma-buf with the (Mesa-internal) sRGB fourcc when its EGL dma-buf format
 * check has been patched to accept it, as in Tirith's Mesa build. Stock Mesa rejects it with
 * EGL_BAD_MATCH, so fall back to plain XRGB8888 once and keep using it. `attrs` must hold the
 * fourcc at index 5 (after EGL_WIDTH, EGL_HEIGHT and their values). */
static EGLint redirect_fourcc = MESA_DRI_IMAGE_FOURCC_SXRGB8888;

static EGLImageKHR create_dma_buf_image(EGLint *attrs) {
    EGLImageKHR image = eglCreateImageKHR_ptr(eglDpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL,
                                              attrs);
    if (image == EGL_NO_IMAGE_KHR && redirect_fourcc != DRM_FORMAT_XRGB8888) {
        fprintf(stderr, "[.] sRGB dma-buf import rejected (0x%04x); using XRGB8888\n",
                eglGetError());
        redirect_fourcc = DRM_FORMAT_XRGB8888;
        attrs[5] = redirect_fourcc;
        image = eglCreateImageKHR_ptr(eglDpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, attrs);
    }
    return image;
}

int offset_fd(int fd) {
    if (in_gramine_vm)
        return fd - GRAPHICS_FS_OFFSET;

    return fd;
}

void setup_egl(void) {
    graphics_ensure_session();
    pthread_mutex_lock(&egl_setup_lock);

    /* Source calls glXMakeCurrent repeatedly even when neither the drawable size nor the
     * redirected context changed. Recreating the EGL context and triple-buffered GBM images on
     * every call leaks their host mappings into the VM's fixed GEM arena. Keep the existing
     * objects for identical-size drawables; genuine startup resizes (32x32 -> 640x480 -> the
     * requested game resolution) are still allowed to rebuild them. */
    if (eglCtx != EGL_NO_CONTEXT && egl_buffer_width == win_width &&
        egl_buffer_height == win_height) {
        if (thread_egl_ctx == EGL_NO_CONTEXT) {
            thread_egl_ctx = eglCreateContext(eglDpy, eglCfg, eglCtx, NULL);
            if (thread_egl_ctx == EGL_NO_CONTEXT) {
                check_egl_error("eglCreateContext(shared)");
                assert(false);
            }
        }
        if (!eglMakeCurrent(eglDpy, EGL_NO_SURFACE, EGL_NO_SURFACE, thread_egl_ctx)) {
            check_egl_error("eglMakeCurrent(reuse)");
            assert(false);
        }
        if (thread_egl_generation != egl_buffer_generation)
            setup_thread_fbos();
        pthread_mutex_unlock(&egl_setup_lock);
        return;
    }

    fprintf(stderr, "========EGL========\n");

    bool first_setup = eglCtx == EGL_NO_CONTEXT;

    if (first_setup) {
        eglGetPlatformDisplayEXT =
            (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
        eglDpy = EGL_NO_DISPLAY;

        if (eglGetPlatformDisplayEXT)
            eglDpy = eglGetPlatformDisplayEXT(EGL_PLATFORM_GBM_KHR, gbm, NULL);
        if (eglDpy == EGL_NO_DISPLAY) {
            eglDpy = eglGetDisplay((EGLNativeDisplayType)NULL);
            printf("No display\n");
            assert(false);
        }
        if (!eglDpy) {
            fprintf(stderr, "eglGetDisplay failed\n");
            assert(false);
        }

        if (!eglInitialize(eglDpy, NULL, NULL)) {
            check_egl_error("eglInitialize");
            assert(false);
        }

        EGLint major, minor;
        eglInitialize(eglDpy, &major, &minor);
        fprintf(stderr, "[.] EGL version %d.%d\n", major, minor);

        EGLint cfgAttrs[] = {EGL_SURFACE_TYPE, EGL_DONT_CARE, // don’t request pbuffer
                            EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_NONE};

        EGLint numCfg = 0;
        if (!eglChooseConfig(eglDpy, cfgAttrs, &eglCfg, 1, &numCfg) || numCfg == 0) {
            fprintf(stderr, "eglChooseConfig failed\n");
            assert(false);
        }

        // Bind desktop GL API
        if (!eglBindAPI(EGL_OPENGL_API)) {
            fprintf(stderr, "eglBindAPI failed\n");
            assert(false);
        }
    }

    /* A drawable resize must not replace the calling thread's GL context: texture/program objects
     * are shared, but bindings and the rest of the GL state are not. Replacing the context here
     * left Source rendering into a valid full-size FBO with an empty state machine. */
    EGLContext ctx = thread_egl_ctx;
    if (ctx == EGL_NO_CONTEXT) {
        ctx = eglCreateContext(eglDpy, eglCfg, first_setup ? EGL_NO_CONTEXT : eglCtx, NULL);
        if (ctx == EGL_NO_CONTEXT) {
            check_egl_error(first_setup ? "eglCreateContext" : "eglCreateContext(shared)");
            assert(false);
        }
    }
    // Make current with no surface (surfaceless)
    if (!eglMakeCurrent(eglDpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) {
        check_egl_error("eglMakeCurrent");
        assert(false);
    }

    eglCreateImageKHR_ptr =
        (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
    eglDestroyImageKHR_ptr =
        (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
    glEGLImageTargetTexture2DOES_ptr =
        (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress(
            "glEGLImageTargetTexture2DOES");

    glGenFramebuffers_ptr = (PFNGLGENFRAMEBUFFERSPROC)eglGetProcAddress("glGenFramebuffers");
    glBindFramebuffer_ptr = (PFNGLBINDFRAMEBUFFERPROC)eglGetProcAddress("glBindFramebuffer");
    glFramebufferTexture2D_ptr = (PFNGLFRAMEBUFFERTEXTURE2DPROC)eglGetProcAddress("glFramebufferTexture2D");
    glFramebufferRenderbuffer_ptr = (PFNGLFRAMEBUFFERRENDERBUFFERPROC)eglGetProcAddress("glFramebufferRenderbuffer");
    glGenRenderbuffers_ptr = (PFNGLGENRENDERBUFFERSPROC)eglGetProcAddress("glGenRenderbuffers");
    glBindRenderbuffer_ptr = (PFNGLBINDRENDERBUFFERPROC)eglGetProcAddress("glBindRenderbuffer");
    glRenderbufferStorage_ptr = (PFNGLRENDERBUFFERSTORAGEPROC)eglGetProcAddress("glRenderbufferStorage");
    glCheckFramebufferStatus_ptr = (PFNGLCHECKFRAMEBUFFERSTATUSPROC)eglGetProcAddress("glCheckFramebufferStatus");
    glFramebufferParameteri_ptr =
        (PFNGLFRAMEBUFFERPARAMETERIPROC)eglGetProcAddress("glFramebufferParameteri");
    glFenceSync_ptr = (PFNGLFENCESYNCPROC)eglGetProcAddress("glFenceSync");
    glClientWaitSync_ptr = (PFNGLCLIENTWAITSYNCPROC)eglGetProcAddress("glClientWaitSync");
    glDeleteSync_ptr = (PFNGLDELETESYNCPROC)eglGetProcAddress("glDeleteSync");

    if (!eglCreateImageKHR_ptr || !eglDestroyImageKHR_ptr ||
        !glEGLImageTargetTexture2DOES_ptr || !glGenFramebuffers_ptr ||
        !glBindFramebuffer_ptr || !glFramebufferTexture2D_ptr ||
        !glFramebufferRenderbuffer_ptr || !glCheckFramebufferStatus_ptr ||
        !glGenRenderbuffers_ptr || !glBindRenderbuffer_ptr ||
        !glRenderbufferStorage_ptr || !glFenceSync_ptr ||
        (!in_gramine_vm && !glFramebufferParameteri_ptr)) {
        fprintf(stderr, "Missing required EGL/GL entry points\n");
        assert(false);
    }
    if (!bufs_pa) {
        void* m = graphics_physmem(sizeof(check) * NUM_BUFFERS, &bufs_pa);
        if (m)
            bufs = m;
        else
            bufs_pa = (uint64_t)(uintptr_t)bufs;
    }
    memset(bufs, 0, sizeof(check) * NUM_BUFFERS);

    for (int i = 0; i < NUM_BUFFERS; ++i) {
        fprintf(stderr, "************************************\n");
        bufs[i].bo = gbm_bo_create(gbm, win_width, win_height, GBM_FORMAT_XRGB8888,
                                    GBM_BO_USE_RENDERING | GBM_BO_USE_SCANOUT |
                                        GBM_BO_USE_LINEAR);

        if (!bufs[i].bo) {
            fprintf(stderr, "gbm_bo_create failed for %d\n", i);
            assert(false);
        }
        bufs[i].bo_fd = gbm_bo_get_fd(bufs[i].bo);
        if (bufs[i].bo_fd < 0) {
            perror("gbm_bo_get_fd");
            assert(false);
        }

        /* The LibOS installs a dma-buf fd returned by a forwarded ioctl as a guest handle at host
         * fd + GRAPHICS_FS_OFFSET (the shared ABI constant); this library and the host listener use
         * the raw host fd (registered below and passed to the listener for the X pixmap), so go
         * back to it. An older LibOS returns the raw host fd (below the offset) itself. */
        if (in_gramine_vm && bufs[i].bo_fd >= (int)GRAPHICS_FS_OFFSET)
            bufs[i].bo_fd -= GRAPHICS_FS_OFFSET;

        /* Register the bo_fd with Saguaro (only needed once, since it remains the same) */
        if (in_gramine_vm) {
            long ret = syscall(syscall_register_host_identity_fd, bufs[i].bo_fd);
            if (ret < 0) {
                fprintf(stderr, "(benign warning) syscall_register_host_identity_fd failed for fd %d: %s\n", 
                    bufs[i].bo_fd, strerror(errno));
            }
        }

        /* Create a duplicate fd to share with x11/xcb */
        uint32_t stride = gbm_bo_get_stride(bufs[i].bo);
        uint32_t size_bytes = stride * win_height;
        int test_fd = dup(bufs[i].bo_fd); 
        fprintf(stderr, "[.] dup() file descriptor for bo_fd=%d of buffer %d\n", bufs[i].bo_fd, i);

        /* These checks are only added for debugging and validation*/
        int flags = fcntl(test_fd, F_GETFD);
        if (flags == -1) {
            perror("[?] test_fd invalid in this process");
        } else {
            fprintf(stderr, "[*] test_fd OK (flags=0x%x)\n", flags);
        }
        test_prime_to_handle(offset_fd(test_fd));

        uint64_t modifier;
        if (!in_gramine_vm) {
            /* Generate pixmap */
            create_pixmap_from_kbuf(bufs, i, size_bytes, stride);

            /* Create an xshmfence and register it as an X sync fence for this pixmap */
            create_xcb_fence(bufs, i);
        } else { 
            /* Send this communication to the host */
            long _offset = acquire_libos_lock();
    comm_page_t* c = comm_page(_offset);
            c->p1 = bufs_pa;
            c->p2 = (uint64_t) i;
            c->p3 = (uint64_t) size_bytes;
            c->p4 = (uint64_t) stride;
            __sync_synchronize();
            c->req_bit = X11_SETUP;
            comm_sync_notify(c);
            relinquish_libos_lock(_offset);
        }

        /* Check for the modifier */
        modifier = gbm_bo_get_modifier(bufs[i].bo);
        if (modifier != DRM_FORMAT_MOD_INVALID) {
            fprintf(stderr, "[*] GBM BO %d has modifier: 0x%lx\n", i, modifier);
            EGLint attrs[] = {
                EGL_WIDTH, win_width,
                EGL_HEIGHT, win_height,
                EGL_LINUX_DRM_FOURCC_EXT, redirect_fourcc,
                EGL_DMA_BUF_PLANE0_FD_EXT, offset_fd(test_fd),
                EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
                EGL_DMA_BUF_PLANE0_PITCH_EXT, stride,
                EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, (EGLint)(modifier & 0xffffffffull),
                EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, (EGLint)(modifier >> 32),
                EGL_NONE
            };

            /* Create an ImageKHR (based on attributes) */
            bufs[i].image = create_dma_buf_image(attrs);
            if (bufs[i].image == EGL_NO_IMAGE_KHR) {
                fprintf(stderr,"[X] eglCreateImageKHR failed for buffer %d\n", i); 
                check_egl_error("eglCreateImageKHR"); 
                assert(0);
            }
        } else {
            fprintf(stderr, "[*] GBM BO %d has no modifier\n", i);

            EGLint attrs[] = {
                EGL_WIDTH, win_width,
                EGL_HEIGHT, win_height,
                EGL_LINUX_DRM_FOURCC_EXT, redirect_fourcc,
                EGL_DMA_BUF_PLANE0_FD_EXT, offset_fd(test_fd),
                EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
                EGL_DMA_BUF_PLANE0_PITCH_EXT, stride,
                EGL_NONE
            };

            /* Create an ImageKHR (based on attributes) */
            bufs[i].image = create_dma_buf_image(attrs);
            if (bufs[i].image == EGL_NO_IMAGE_KHR) {
                fprintf(stderr,"[X] eglCreateImageKHR failed for buffer %d\n", i); 
                check_egl_error("eglCreateImageKHR"); 
                assert(0);
            }
        }

        /* Create GL texture and bind the image to it */
        glGenTextures(1, &bufs[i].tex);
        glBindTexture(GL_TEXTURE_2D, bufs[i].tex);
        glEGLImageTargetTexture2DOES_ptr(GL_TEXTURE_2D, (GLeglImageOES)bufs[i].image);
        glGenRenderbuffers_ptr(1, &bufs[i].rbo_depth);
        glBindRenderbuffer_ptr(GL_RENDERBUFFER, bufs[i].rbo_depth);
        glRenderbufferStorage_ptr(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, win_width, win_height);

        /* Create a dedicated FBO for this buffer to avoid per-frame reattachment */
        glGenFramebuffers_ptr(1, &bufs[i].fbo);
        glBindFramebuffer_ptr(GL_FRAMEBUFFER, bufs[i].fbo);
        configure_redirect_orientation();
        glFramebufferTexture2D_ptr(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, bufs[i].tex, 0);
        glFramebufferRenderbuffer_ptr(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, bufs[i].rbo_depth);
        GLenum status = glCheckFramebufferStatus_ptr(GL_FRAMEBUFFER);
        if (status != GL_FRAMEBUFFER_COMPLETE) {
            fprintf(stderr, "FBO incomplete for buffer %d: 0x%x\n", i, status);
            assert(false);
        }
    }

    glGenFramebuffers_ptr(1, &fbo);
    if (first_setup)
        eglCtx = ctx;
    thread_egl_ctx = ctx;
    egl_buffer_width = win_width;
    egl_buffer_height = win_height;
    egl_buffer_generation++;
    for (int i = 0; i < NUM_BUFFERS; ++i)
        thread_fbos[i] = bufs[i].fbo;
    thread_egl_generation = egl_buffer_generation;
    fprintf(stderr, "[*] framebuffer object: %d\n", fbo);
    fprintf(stderr, "========EGL========\n\n");
    pthread_mutex_unlock(&egl_setup_lock);
}
