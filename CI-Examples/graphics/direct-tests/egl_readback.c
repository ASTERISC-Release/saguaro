/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/*
 * egl_readback: GPU rendering without a window.
 *  1. EGL on GBM, a surfaceless OpenGL ES context: clear a renderbuffer, read the pixels back.
 *  2. dma-buf round trip: export a GBM buffer object as a dma-buf fd (PRIME), import that fd into
 *     EGL as an image (PRIME import), clear it on the GPU, then check the pixels through a CPU
 *     mapping of the dma-buf itself (mmap of the fd, DMA_BUF_IOCTL_SYNC) and of the buffer object.
 * Usage: egl_readback [/dev/dri/renderD128]. Prints "TEST OK" on success.
 */

#define _GNU_SOURCE
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <drm_fourcc.h>
#include <errno.h>
#include <fcntl.h>
#include <gbm.h>
#include <linux/dma-buf.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#define W 256
#define H 128

static int fail(const char* what) {
    printf("%s failed (EGL 0x%x, GL 0x%x, errno %s)\n", what, eglGetError(), glGetError(),
           strerror(errno));
    return 1;
}

static int check_pixels(const uint8_t* p, size_t stride, const uint8_t want[4], const char* what) {
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            const uint8_t* px = p + (size_t)y * stride + (size_t)x * 4;
            if (memcmp(px, want, 4)) {
                printf("%s: pixel %d,%d is %02x %02x %02x %02x, expected %02x %02x %02x %02x\n",
                       what, x, y, px[0], px[1], px[2], px[3], want[0], want[1], want[2], want[3]);
                return 1;
            }
        }
    printf("%s: %dx%d pixels OK\n", what, W, H);
    return 0;
}

int main(int argc, char** argv) {
    const char* node = argc > 1 ? argv[1] : "/dev/dri/renderD128";
    int fd = open(node, O_RDWR | O_CLOEXEC);
    if (fd < 0)
        return fail("open");
    struct gbm_device* gbm = gbm_create_device(fd);
    if (!gbm)
        return fail("gbm_create_device");

    EGLDisplay dpy = eglGetPlatformDisplay(EGL_PLATFORM_GBM_KHR, gbm, NULL);
    if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, NULL, NULL))
        return fail("eglInitialize");
    printf("EGL vendor %s, version %s\n", eglQueryString(dpy, EGL_VENDOR),
           eglQueryString(dpy, EGL_VERSION));
    if (!eglBindAPI(EGL_OPENGL_ES_API))
        return fail("eglBindAPI");
    static const EGLint ctx_attrs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    EGLContext ctx = eglCreateContext(dpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, ctx_attrs);
    if (ctx == EGL_NO_CONTEXT)
        return fail("eglCreateContext");
    if (!eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx))
        return fail("eglMakeCurrent (surfaceless)");
    printf("GL renderer %s\n", glGetString(GL_RENDERER));

    /* 1. renderbuffer, clear, glReadPixels */
    GLuint fbo, rb;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glGenRenderbuffers(1, &rb);
    glBindRenderbuffer(GL_RENDERBUFFER, rb);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8_OES, W, H);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        return fail("renderbuffer framebuffer");
    glViewport(0, 0, W, H);
    glClearColor(1.0f, 0.5f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    static uint8_t pixels[W * H * 4];
    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    if (glGetError() != GL_NO_ERROR)
        return fail("glReadPixels");
    const uint8_t want1[4] = {0xff, 0x80, 0x00, 0xff};
    if (check_pixels(pixels, W * 4, want1, "renderbuffer readback"))
        return 1;

    /* 2. dma-buf round trip */
    struct gbm_bo* bo = gbm_bo_create(gbm, W, H, GBM_FORMAT_ABGR8888,
                                      GBM_BO_USE_RENDERING | GBM_BO_USE_LINEAR);
    if (!bo)
        return fail("gbm_bo_create");
    int dmabuf = gbm_bo_get_fd(bo); /* DRM_IOCTL_PRIME_HANDLE_TO_FD */
    if (dmabuf < 0)
        return fail("gbm_bo_get_fd");
    off_t size = lseek(dmabuf, 0, SEEK_END);
    uint32_t stride = gbm_bo_get_stride(bo);
    printf("dma-buf fd %d, size %lld, stride %u\n", dmabuf, (long long)size, stride);
    if (size < (off_t)stride * H)
        return fail("lseek(SEEK_END) of the dma-buf");
    lseek(dmabuf, 0, SEEK_SET);

    PFNEGLCREATEIMAGEKHRPROC create_image =
        (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC image_target_texture =
        (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES");
    const EGLint img_attrs[] = {
        EGL_WIDTH, W, EGL_HEIGHT, H, EGL_LINUX_DRM_FOURCC_EXT, DRM_FORMAT_ABGR8888,
        EGL_DMA_BUF_PLANE0_FD_EXT, dmabuf, EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
        EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)stride, EGL_NONE,
    };
    /* DRM_IOCTL_PRIME_FD_TO_HANDLE */
    EGLImageKHR img = create_image(dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, img_attrs);
    if (img == EGL_NO_IMAGE_KHR)
        return fail("eglCreateImageKHR from the dma-buf");
    GLuint tex, fbo2;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    image_target_texture(GL_TEXTURE_2D, img);
    glGenFramebuffers(1, &fbo2);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo2);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        return fail("dma-buf framebuffer");
    glClearColor(0.0f, 0.25f, 1.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glFinish();

    /* the GPU is done after glFinish; poll on the dma-buf waits for its fences too */
    struct pollfd pfd = {.fd = dmabuf, .events = POLLIN};
    if (poll(&pfd, 1, 5000) != 1 || !(pfd.revents & POLLIN))
        return fail("poll on the dma-buf");

    const uint8_t want2[4] = {0x00, 0x40, 0xff, 0xff};
    uint8_t* map = mmap(NULL, (size_t)size, PROT_READ, MAP_SHARED, dmabuf, 0);
    if (map == MAP_FAILED)
        return fail("mmap of the dma-buf");
    struct dma_buf_sync sync = {.flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ};
    if (ioctl(dmabuf, DMA_BUF_IOCTL_SYNC, &sync) < 0)
        return fail("DMA_BUF_IOCTL_SYNC start");
    int bad = check_pixels(map, stride, want2, "dma-buf mapping");
    sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ;
    if (ioctl(dmabuf, DMA_BUF_IOCTL_SYNC, &sync) < 0)
        return fail("DMA_BUF_IOCTL_SYNC end");
    munmap(map, (size_t)size);
    if (bad)
        return 1;

    /* a second fd for the same buffer (dup) still names it */
    int dup_fd = dup(dmabuf);
    struct gbm_import_fd_data imp = {.fd = dup_fd, .width = W, .height = H, .stride = stride,
                                     .format = GBM_FORMAT_ABGR8888};
    struct gbm_bo* bo2 = gbm_bo_import(gbm, GBM_BO_IMPORT_FD, &imp, GBM_BO_USE_RENDERING);
    if (!bo2)
        return fail("gbm_bo_import of the dup'ed dma-buf");
    uint32_t stride2 = 0;
    void* map_data = NULL;
    uint8_t* p2 = gbm_bo_map(bo2, 0, 0, W, H, GBM_BO_TRANSFER_READ, &stride2, &map_data);
    if (!p2)
        return fail("gbm_bo_map of the imported buffer");
    bad = check_pixels(p2, stride2, want2, "imported buffer object");
    gbm_bo_unmap(bo2, map_data);
    if (bad)
        return 1;

    gbm_bo_destroy(bo2);
    close(dup_fd);
    close(dmabuf);
    gbm_bo_destroy(bo);
    eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(dpy, ctx);
    eglTerminate(dpy);
    gbm_device_destroy(gbm);
    close(fd);
    printf("TEST OK\n");
    return 0;
}
