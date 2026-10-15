/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/*
 * egl_surfaceless: GPU rendering through EGL's surfaceless platform (EGL_MESA_platform_surfaceless,
 * offered by Mesa and by the NVIDIA driver), no GBM, no render node, no window. Clears a 1920x1080
 * renderbuffer to a known color and reads it back, then draws full-screen quads with a heavier
 * fragment shader (as gl_bench) and prints the rate. Prints "TEST OK" when the readback matches.
 * Usage: egl_surfaceless [frames] [draws per frame]
 */

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define W 1920
#define H 1080

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static GLuint shader(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    return s;
}

int main(int argc, char** argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    int frames = argc > 1 ? atoi(argv[1]) : 300;
    int draws = argc > 2 ? atoi(argv[2]) : 20;

    EGLDisplay dpy = eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
    if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, NULL, NULL)) {
        printf("EGL setup failed (0x%x)\n", eglGetError());
        return 1;
    }
    eglBindAPI(EGL_OPENGL_ES_API);
    static const EGLint cfg_attrs[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE,
                                       EGL_OPENGL_ES2_BIT, EGL_NONE};
    EGLConfig cfg;
    EGLint ncfg = 0;
    if (!eglChooseConfig(dpy, cfg_attrs, &cfg, 1, &ncfg) || ncfg < 1)
        cfg = EGL_NO_CONFIG_KHR;
    static const EGLint ctx_attrs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attrs);
    if (ctx == EGL_NO_CONTEXT || !eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) {
        printf("context failed (0x%x)\n", eglGetError());
        return 1;
    }
    printf("EGL vendor: %s, GL renderer: %s\n", eglQueryString(dpy, EGL_VENDOR),
           (const char*)glGetString(GL_RENDERER));

    GLuint fbo, rb;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glGenRenderbuffers(1, &rb);
    glBindRenderbuffer(GL_RENDERBUFFER, rb);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8_OES, W, H);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        printf("framebuffer incomplete\n");
        return 1;
    }
    glViewport(0, 0, W, H);

    /* readback check */
    glClearColor(0.0f, 1.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    static uint8_t px[4 * 64];
    glReadPixels(W / 2, H / 2, 64, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    for (int i = 0; i < 64; i++) {
        if (px[4 * i] != 0 || px[4 * i + 1] != 255 || px[4 * i + 2] != 0) {
            printf("readback mismatch at %d: %u %u %u\n", i, px[4 * i], px[4 * i + 1], px[4 * i + 2]);
            return 1;
        }
    }

    GLuint prog = glCreateProgram();
    glAttachShader(prog, shader(GL_VERTEX_SHADER,
        "attribute vec2 p; varying vec2 uv; void main(){ uv = p; gl_Position = vec4(p, 0.0, 1.0); }"));
    glAttachShader(prog, shader(GL_FRAGMENT_SHADER,
        "precision mediump float; varying vec2 uv; uniform float t;\n"
        "void main(){ vec2 z = uv; float a = 0.0;\n"
        "  for (int i = 0; i < 16; i++) { z = vec2(z.x*z.x - z.y*z.y, 2.0*z.x*z.y) + uv*0.7 + t; a += length(z); }\n"
        "  gl_FragColor = vec4(fract(a), fract(a*0.5), fract(a*0.25), 1.0); }"));
    glBindAttribLocation(prog, 0, "p");
    glLinkProgram(prog);
    glUseProgram(prog);
    GLint ut = glGetUniformLocation(prog, "t");
    static const float quad[] = {-1, -1, 1, -1, -1, 1, 1, 1};
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, quad);
    glEnableVertexAttribArray(0);

    uint64_t t0 = 0;
    for (int f = -10; f < frames; f++) { /* 10 warm-up frames */
        if (f == 0)
            t0 = now_ns();
        for (int d = 0; d < draws; d++) {
            glUniform1f(ut, (float)(f * draws + d) * 1e-4f);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        }
        glFinish();
    }
    uint64_t t1 = now_ns();
    if (glGetError() != GL_NO_ERROR) {
        printf("GL error\n");
        return 1;
    }
    printf("%.1f frames/s (%d frames, %d draws each, %dx%d)\n", frames / ((t1 - t0) / 1e9), frames,
           draws, W, H);
    printf("TEST OK\n");
    return 0;
}
