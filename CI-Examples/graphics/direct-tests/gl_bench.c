/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/*
 * gl_bench: offscreen GPU throughput without presentation (EGL on GBM, surfaceless GLES2):
 * draws full-screen quads with a moderately heavy fragment shader into a 1920x1080 renderbuffer,
 * many draws per frame, glFinish() per frame. Separates the GPU work from the X presentation
 * path when comparing native and saguaro-direct runs.
 * Usage: gl_bench [/dev/dri/renderD128] [frames] [draws per frame]
 */

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <fcntl.h>
#include <gbm.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

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
    const char* node = argc > 1 ? argv[1] : "/dev/dri/renderD128";
    int frames = argc > 2 ? atoi(argv[2]) : 300;
    int draws = argc > 3 ? atoi(argv[3]) : 20;
    int fd = open(node, O_RDWR | O_CLOEXEC);
    struct gbm_device* gbm = fd >= 0 ? gbm_create_device(fd) : NULL;
    EGLDisplay dpy = gbm ? eglGetPlatformDisplay(EGL_PLATFORM_GBM_KHR, gbm, NULL) : EGL_NO_DISPLAY;
    if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, NULL, NULL)) {
        printf("EGL setup failed\n");
        return 1;
    }
    eglBindAPI(EGL_OPENGL_ES_API);
    static const EGLint ctx_attrs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    EGLContext ctx = eglCreateContext(dpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, ctx_attrs);
    if (ctx == EGL_NO_CONTEXT || !eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) {
        printf("context failed\n");
        return 1;
    }

    GLuint fbo, rb;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glGenRenderbuffers(1, &rb);
    glBindRenderbuffer(GL_RENDERBUFFER, rb);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8_OES, 1920, 1080);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb);
    glViewport(0, 0, 1920, 1080);

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
    printf("%s: %.1f frames/s (%d frames, %d draws each, 1920x1080)\n",
           (const char*)glGetString(GL_RENDERER), frames / ((t1 - t0) / 1e9), frames, draws);
    return 0;
}
