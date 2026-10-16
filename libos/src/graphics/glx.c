/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

#define _GNU_SOURCE
#include <GL/gl.h>
#include <GL/glx.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_syswm.h>
#include <assert.h>
#include <dlfcn.h>
#include <link.h>
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

#include <dlfcn.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Uncomment this definition to forward to glX, instead of masking */
// #define NATIVE_GLX

extern PFNGLFENCESYNCPROC glFenceSync_ptr;
extern PFNGLDELETESYNCPROC glDeleteSync_ptr;
extern PFNGLCLIENTWAITSYNCPROC glClientWaitSync_ptr;
extern PFNGLFRAMEBUFFERTEXTURE2DPROC glFramebufferTexture2D_ptr;
extern PFNGLBINDFRAMEBUFFERPROC glBindFramebuffer_ptr;
extern PFNGLBINDRENDERBUFFERPROC glBindRenderbuffer_ptr;

dlsym_fn_t real_dlsym;
static uint64_t frame_count = 0;
static Bool (*glXMakeCurrent_ptr)(Display *dpy, GLXDrawable drawable, GLXContext ctx1) = NULL;
static _Thread_local Display *current_glx_display;
static _Thread_local GLXContext current_glx_context;
static _Thread_local GLXDrawable current_glx_drawable;

void resolve_dlsym(void);


static uint64_t cpu_latency_sum = 0;
static uint64_t cpu_latency_count = 0;
static struct timespec prev_swap_end = {0, 0};

static const char* glx_extensions =
    "GLX_ARB_create_context GLX_ARB_create_context_no_error GLX_ARB_create_context_profile "
    "GLX_ARB_create_context_robustness GLX_ARB_get_proc_address GLX_EXT_create_context_es2_profile "
    "GLX_EXT_create_context_es_profile GLX_EXT_visual_info";

static int glx_major                 = 1;
static int glx_minor                 = 4;
static const char* glx_majmin_string = "1.4";

static int max_width, max_height, real_width, real_height;
static unsigned long start = 0;
static void* swap_sync_lock;
static bool gpu_mem_checked = false;
static int frame_probe_enabled = -1;

static void probe_current_framebuffer(void) {
    if (frame_probe_enabled < 0) {
        const char *value = getenv("EGL_FRAME_PROBE");
        frame_probe_enabled = value && strcmp(value, "1") == 0;
    }
    if (!frame_probe_enabled || (frame_count >= 8 && frame_count % 120 != 0))
        return;

    GLint framebuffer = 0;
    GLint viewport[4] = {0};
    GLubyte samples[5][4] = {{0}};
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &framebuffer);
    glGetIntegerv(GL_VIEWPORT, viewport);

    const GLint points[5][2] = {
        {0, 0},
        {viewport[2] > 0 ? viewport[2] - 1 : 0, 0},
        {0, viewport[3] > 0 ? viewport[3] - 1 : 0},
        {viewport[2] > 0 ? viewport[2] - 1 : 0,
         viewport[3] > 0 ? viewport[3] - 1 : 0},
        {viewport[2] / 2, viewport[3] / 2},
    };

    glFinish();
    for (size_t i = 0; i < 5; ++i)
        glReadPixels(points[i][0], points[i][1], 1, 1, GL_RGBA,
                     GL_UNSIGNED_BYTE, samples[i]);

    fprintf(stderr,
            "[frame-probe] frame=%" PRIu64 " fbo=%d viewport=%d,%d %dx%d "
            "rgba=%u,%u,%u,%u/%u,%u,%u,%u/%u,%u,%u,%u/%u,%u,%u,%u/%u,%u,%u,%u "
            "error=0x%x\n",
            frame_count, framebuffer, viewport[0], viewport[1], viewport[2], viewport[3],
            samples[0][0], samples[0][1], samples[0][2], samples[0][3],
            samples[1][0], samples[1][1], samples[1][2], samples[1][3],
            samples[2][0], samples[2][1], samples[2][2], samples[2][3],
            samples[3][0], samples[3][1], samples[3][2], samples[3][3],
            samples[4][0], samples[4][1], samples[4][2], samples[4][3], glGetError());
}

/* Provide safe no-op fallbacks for ICD macros if they're not defined elsewhere. */
#ifndef ICD_SET_MAX_DIMENSIONS_DEFINITION
#define ICD_SET_MAX_DIMENSIONS_DEFINITION(a, b, c, d)
#endif
#ifndef ICD_RESIZE_DEFINITION
#define ICD_RESIZE_DEFINITION(a, b)
#endif

ICD_SET_MAX_DIMENSIONS_DEFINITION(max_width, max_height, real_width, real_height);
ICD_RESIZE_DEFINITION(real_width, real_height);

struct glx_swap_data {
    XVisualInfo vinfo;
    XVisualInfo* visual_list;
    XVisualInfo visual_template;
    int nxvisuals;
    Window parent;

    int width, height;
    XImage* ximage;
    XEvent event;

    XGCValues gcv;
    GC gc;

    bool initialized;
};

struct glx_fb_config {
    int render_type, drawable_type, double_buffer, red_size, green_size, blue_size, alpha_size,
        stencil_size, depth_size, accum_red_size, accum_green_size, accum_blue_size,
        accum_alpha_size, accum_stencil_size, renderable, visual_type;
    // to-do: add more like stereo
};

static int n_valid_fb_configs;
static bool glx_logging_enabled;
static bool dlsym_logging_enabled;
static struct glx_fb_config fb_configs[1729] = {0};
static int fb_valid_color_sizes[] = {0, 1, 8, 16, 24, 32};
static int fb_valid_render_types[] = {GLX_RGBA_BIT, GLX_COLOR_INDEX_BIT};
static int fb_valid_doublebuffer_types[] = {False, True};
static int fb_valid_drawable_types[] = {GLX_WINDOW_BIT};
static int fb_valid_visual_types[] = {GLX_TRUE_COLOR,   GLX_DIRECT_COLOR, GLX_PSEUDO_COLOR,
                                      GLX_STATIC_COLOR, GLX_GRAY_SCALE,   GLX_STATIC_GRAY};

#define ARR_SIZE(x) (sizeof(x) / sizeof(*x))
static void glx_generate_fb_configs() {
    int idx = 0;
    for (int a = 0; a < ARR_SIZE(fb_valid_color_sizes); a++) {       // varying color size
        for (int b = 0; b < ARR_SIZE(fb_valid_render_types); b++) {  // varying render types
            for (int c = 0; c < ARR_SIZE(fb_valid_doublebuffer_types);
                 c++) {  // varying double buffer
                for (int d = 0; d < ARR_SIZE(fb_valid_drawable_types);
                     d++) {  // varying drawable type
                    for (int e = 0; e < ARR_SIZE(fb_valid_doublebuffer_types);
                         e++) {  // varying renderable
                        for (int f = 0; f < ARR_SIZE(fb_valid_visual_types);
                             f++) {  // varying visual type
                            for (int g = 0; g < ARR_SIZE(fb_valid_color_sizes);
                                 g++) {  // varying depth
                                fb_configs[idx].red_size         = fb_valid_color_sizes[a];
                                fb_configs[idx].green_size       = fb_valid_color_sizes[a];
                                fb_configs[idx].blue_size        = fb_valid_color_sizes[a];
                                fb_configs[idx].alpha_size       = fb_valid_color_sizes[a];
                                fb_configs[idx].accum_red_size   = fb_valid_color_sizes[a];
                                fb_configs[idx].accum_green_size = fb_valid_color_sizes[a];
                                fb_configs[idx].accum_blue_size  = fb_valid_color_sizes[a];
                                fb_configs[idx].accum_alpha_size = fb_valid_color_sizes[a];

                                fb_configs[idx].stencil_size = -1;  // fb_valid_color_sizes[a];
                                fb_configs[idx].depth_size   = fb_valid_color_sizes[g];

                                fb_configs[idx].render_type   = fb_valid_render_types[b];
                                fb_configs[idx].double_buffer = fb_valid_doublebuffer_types[c];
                                fb_configs[idx].drawable_type = fb_valid_drawable_types[d];

                                fb_configs[idx].renderable  = fb_valid_doublebuffer_types[e];
                                fb_configs[idx].visual_type = fb_valid_visual_types[f];

                                idx++;
                            }
                        }
                    }
                }
            }
        }
    }

    n_valid_fb_configs = idx;
}

static void __attribute__((constructor)) glximpl_init(void) {
    const char* log_enabled = getenv("EGL_GLX_LOG");
    glx_logging_enabled = log_enabled && strcmp(log_enabled, "1") == 0;
    const char* dlsym_log_enabled = getenv("EGL_DLSYM_LOG");
    dlsym_logging_enabled = dlsym_log_enabled && strcmp(dlsym_log_enabled, "1") == 0;

    const char* version_override = getenv("GLX_VERSION_OVERRIDE");
    if (version_override && strlen(version_override) >= 3 && version_override[1] == '.') {
        glx_majmin_string = version_override;
        glx_major = version_override[0] - '0';
        glx_minor = version_override[2] - '0';
    }

    glx_generate_fb_configs();
    if (glx_logging_enabled)
        fprintf(stderr, "[glx] initialized %d framebuffer configurations\n", n_valid_fb_configs);
}
#undef ARR_SIZE

static _Thread_local GLuint current_fbo = 0;

static void gpu_memory_sanity_check(void) {
    if (gpu_mem_checked)
        return;

    GLint vp[4] = {0};
    glGetIntegerv(GL_VIEWPORT, vp);
    int vp_w = vp[2] > 0 ? vp[2] : win_width;
    int vp_h = vp[3] > 0 ? vp[3] : win_height;
    if (vp_w <= 0 || vp_h <= 0)
        return;

    /* Allow user to choose block size; default small to minimize artifacts. */
    int w = 4, h = 4;
    const char *full = getenv("EGL_SANITY_FULL");
    const char *blk  = getenv("EGL_SANITY_BLOCK");
    if (full && strcmp(full, "1") == 0) {
        w = vp_w;
        h = vp_h;
    } else if (blk) {
        int v = atoi(blk);
        if (v > 0) {
            w = v;
            h = v;
        }
    }

    if (w > vp_w) w = vp_w;
    if (h > vp_h) h = vp_h;

    GLboolean scissor_enabled = glIsEnabled(GL_SCISSOR_TEST);
    GLint scissor_box[4] = {0};
    glGetIntegerv(GL_SCISSOR_BOX, scissor_box);

    GLfloat prev_clear[4] = {0};
    glGetFloatv(GL_COLOR_CLEAR_VALUE, prev_clear);

    glEnable(GL_SCISSOR_TEST);
    glScissor(0, 0, w, h);

    const float cr = 0.10f, cg = 0.20f, cb = 0.30f, ca = 0.40f;
    glClearColor(cr, cg, cb, ca);
    glClear(GL_COLOR_BUFFER_BIT);
    glFinish();

    size_t pix_bytes = (size_t)w * (size_t)h * 4;
    uint8_t *pixels = malloc(pix_bytes);
    if (!pixels) {
        perror("malloc pixels for sanity check");
        /* best effort: restore state and bail */
        glClearColor(prev_clear[0], prev_clear[1], prev_clear[2], prev_clear[3]);
        glScissor(scissor_box[0], scissor_box[1], scissor_box[2], scissor_box[3]);
        if (!scissor_enabled)
            glDisable(GL_SCISSOR_TEST);
        return;
    }

    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, pixels);

    const uint8_t expect[3] = {
        (uint8_t)lroundf(cr * 255.0f),
        (uint8_t)lroundf(cg * 255.0f),
        (uint8_t)lroundf(cb * 255.0f),
    };

    int first_mismatch = -1;
    for (int i = 0; i < w * h; ++i) {
        for (int c = 0; c < 3; ++c) {
            uint8_t v = pixels[i * 4 + c];
            if (abs((int)v - (int)expect[c]) > 1) { /* allow +/-1 rounding */
                first_mismatch = i;
                break;
            }
        }
        if (first_mismatch >= 0)
            break;
    }

    if (first_mismatch < 0) {
        fprintf(stderr, "[sanity] GPU/CPU view of BO agrees on %dx%d block\n", w, h);
    } else {
        int p = first_mismatch * 4;
        fprintf(stderr, "[sanity] Mismatch in GPU/CPU BO view at pixel %d (expected rgb=%u,%u,%u got rgb=%u,%u,%u; alpha ignored)\n",
                first_mismatch,
                expect[0], expect[1], expect[2],
                pixels[p + 0], pixels[p + 1], pixels[p + 2]);
    }

    free(pixels);

    glClearColor(prev_clear[0], prev_clear[1], prev_clear[2], prev_clear[3]);
    glScissor(scissor_box[0], scissor_box[1], scissor_box[2], scissor_box[3]);
    if (!scissor_enabled)
        glDisable(GL_SCISSOR_TEST);

    gpu_mem_checked = true;
}

void glBindFramebuffer(GLenum target, GLuint framebuffer) {
    // Lazy resolve the real symbol once
    if (!glBindFramebuffer_ptr) {
        glBindFramebuffer_ptr = dlsym(RTLD_NEXT, "glBindFramebuffer");
        if (!glBindFramebuffer_ptr) {
            fprintf(stderr, "[hook] Failed to resolve real glBindFramebuffer()\n");
            return;
        }
    }

    if (framebuffer == 0) {
        /* App is binding the default framebuffer, redirect it to our per-buffer FBO */
        GLuint redirect_fbo = get_redirect_fbo(cur);
        if (!redirect_fbo)
            redirect_fbo = bufs[cur].fbo;
        if (current_fbo != redirect_fbo) {
            if (!in_gramine_vm) {
                cur = query_idle_fence_buffer(bufs, cur);
#ifdef IDLE_FENCE_WAIT
            } else {
                /* Ask the host which redirected buffer is no longer in use.
                 * This path is omitted entirely in the default build, where
                 * query_idle_fence_buffer() is also a no-op. */
                long _offset = acquire_libos_lock();
                comm_page_t* c = comm_page(_offset);
                c->p1 = bufs_pa;
                c->p2 = (uint64_t)cur;
                __sync_synchronize();

                c->req_bit = X11_IDLE_FENCE_BUFFER;
                comm_sync_notify(c);
                cur = c->ret;
                relinquish_libos_lock(_offset);
#endif
            }

            redirect_fbo = get_redirect_fbo(cur);
            if (!redirect_fbo)
                redirect_fbo = bufs[cur].fbo;
            glBindFramebuffer_ptr(target, redirect_fbo);
            current_fbo = redirect_fbo;
        }

        return;
    }

    if (current_fbo != framebuffer) {
        glBindFramebuffer_ptr(target, framebuffer);
        current_fbo = framebuffer;
    }

}

/* Defined in xcb.c */
void xcb_present(check *bufs, int cur);

static void update_window_size_from_viewport(GLXDrawable drawable) {
    GLint viewport[4] = {0};
    glGetIntegerv(GL_VIEWPORT, viewport);
    if (viewport[2] <= 0 || viewport[3] <= 0 ||
        (viewport[2] == win_width && viewport[3] == win_height))
        return;

    win_width = viewport[2];
    win_height = viewport[3];
    fprintf(stderr, "Using viewport size: %dx%d\n", win_width, win_height);
    if (in_gramine_vm)
        update_vm_window(drawable);
    else
        update_window();
}

void glXSwapBuffers(Display *dpy, GLXDrawable drawable) {
    graphics_ensure_session();
    if (glx_logging_enabled)
        fprintf(stderr, "[glx] glXSwapBuffers(drawable=%lu) begin\n",
                (unsigned long)drawable);
    #ifdef BENCHMARKING
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    if (prev_swap_end.tv_sec != 0 || prev_swap_end.tv_nsec != 0) {
        uint64_t cpu_ns = (now.tv_sec - prev_swap_end.tv_sec) * 1000000000ULL +
                          (now.tv_nsec - prev_swap_end.tv_nsec);
        cpu_latency_sum += cpu_ns;
        cpu_latency_count++;
        if (cpu_latency_count >= STATS_INTERVAL) {
            double avg_cpu_ms = (cpu_latency_sum / cpu_latency_count) / 1000000.0;
            fprintf(stderr, "[cpu_latency] Avg over %lu frames: %.2f ms\n",
                    cpu_latency_count, avg_cpu_ms);
            cpu_latency_sum = 0;
            cpu_latency_count = 0;
        }
    }

    #endif
    
    /* SDL may resize the X window after the final glXMakeCurrent call. Source then updates its
     * viewport, but the redirector would keep presenting the earlier 640x480 GBM buffers into a
     * 1920x1080 window. Read the local GL viewport at swap time and rebuild once its resize has
     * landed. Avoid a synchronous X11 query here: that VM round trip can stall an otherwise
     * continuously rendering application until another X event arrives. The frame already
     * rendered into the old-size buffer is discarded. */
    int old_width = win_width;
    int old_height = win_height;
    /* SDL_CreateWindow can expose the real X11 parent after an internal
     * glXMakeCurrent has already sent a GLX drawable ID to the listener. Send
     * the native parent even when the viewport size itself did not change. */
    if (in_gramine_vm && consume_guest_app_window_change())
        update_vm_window(drawable);
    update_window_size_from_viewport(drawable);
    if (win_width != old_width || win_height != old_height) {
        GLint active_texture = GL_TEXTURE0;
        GLint texture_binding = 0;
        GLint renderbuffer_binding = 0;
        glGetIntegerv(GL_ACTIVE_TEXTURE, &active_texture);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture_binding);
        glGetIntegerv(GL_RENDERBUFFER_BINDING, &renderbuffer_binding);

        setup_egl();

        glActiveTexture((GLenum)active_texture);
        glBindTexture(GL_TEXTURE_2D, (GLuint)texture_binding);
        glBindRenderbuffer_ptr(GL_RENDERBUFFER, (GLuint)renderbuffer_binding);
        current_fbo = ~(GLuint)0;
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        fprintf(stderr, "[glx] rebuilt redirect buffers at %dx%d after drawable resize\n",
                win_width, win_height);
        return;
    }

    probe_current_framebuffer();
    glFlush();

    if (false) {
        glFinish(); 
        static unsigned char* pixels;

        GLint vp[4];
        glGetIntegerv(GL_VIEWPORT, vp);
        int w = vp[2];
        int h = vp[3];

        pixels = malloc(w * h * 4);
        if (pixels) {
            glReadBuffer(GL_BACK);
            glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, pixels);

            char fname[64];
            snprintf(fname, sizeof(fname), "png_logs/frame_%ld.ppm", frame_count);
            dump_ppm(fname, w, h, pixels);
            
            free(pixels);
            fprintf(stderr, "[.] Dumped frame(s) to %s (%dx%d)\n", fname, w, h);
        }
    }    

    if (!in_gramine_vm) {
        /* Process */
        xcb_present(bufs, cur);
    } else {
        /* Gramine VM */
        /* Send message to the host */
        long _offset = acquire_libos_lock();
        comm_page_t* c = comm_page(_offset);
        c->p1          = bufs_pa;
        c->p2          = (uint64_t)cur;
        __sync_synchronize();

        c->req_bit     = X11_PRESENT;
        comm_sync_notify(c);
        relinquish_libos_lock(_offset);
    }

    cur = (cur + 1) % NUM_BUFFERS;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);


    frame_count++;
    if (ioctl_logging_enabled && frame_count % 5000 == 0) {
        if (ioctl_count > 0) {
            /* Print IOCTL statistics collected since last report and reset counters */
            print_ioctl_stats();
        }
    }

    #ifdef BENCHMARKING
        struct timespec now_end;
        clock_gettime(CLOCK_MONOTONIC, &now_end);
        prev_swap_end = now_end;
    #endif

    if (glx_logging_enabled)
        fprintf(stderr, "[glx] glXSwapBuffers end\n");
}

void glXSwapIntervalEXT(Display *d, GLXDrawable draw, int interval) { eglSwapInterval(eglDpy, interval); }

Bool glXMakeCurrent(Display *dpy, GLXDrawable drawable, GLXContext ctx1) {
    graphics_ensure_session();
    if (glx_logging_enabled)
        fprintf(stderr, "[glx] glXMakeCurrent(drawable=%lu, context=%p)\n",
                (unsigned long)drawable, ctx1);
    if (!glXMakeCurrent_ptr) {
        glXMakeCurrent_ptr = dlsym(RTLD_NEXT, "glXMakeCurrent");
        if (!glXMakeCurrent_ptr) {
            fprintf(stderr, "[hook] Failed to resolve real glXMakeCurrent()\n");
            return False;
        }
    }
    current_glx_display = ctx1 ? dpy : NULL;
    current_glx_context = ctx1;
    current_glx_drawable = ctx1 ? drawable : None;
    if (!in_gramine_vm)
        use_native_presentation_window(drawable);
    update_window_size_from_drawable(dpy, drawable);

    /* Setting up EGL */
    setup_egl();
    /* FBO object names are context-local and are commonly reused numerically by Mesa. Force the
     * default-framebuffer redirect to bind the FBO belonging to the context made current above. */
    current_fbo = ~(GLuint)0;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    // gpu_memory_sanity_check();

    /* Disable logging */
    // set_syscall_logging(0);

    return true;
}

static void* glx_get_proc_address_impl(const GLubyte *s) {
    void *addr;
    static void *my_handle;
    const char *name = (const char *)s;

    if (glx_logging_enabled)
        fprintf(stderr, "[glx] glXGetProcAddressARB(%s)\n", name);

    /* to-do: use stripped str? */
    if (strstr(name, "glX") || strstr(name, "glBindFramebuffer") ||
        strstr(name, "glXSwapIntervalEXT")) {
        addr = dlsym(NULL, name);
        return addr;
    }

    if (!my_handle) {
        my_handle = dlopen("libGL.so.1", RTLD_NOW | RTLD_GLOBAL);
    }
    resolve_dlsym();
    addr = my_handle ? real_dlsym(my_handle, name) : NULL;
    if (!addr) {
        /* Extension entry points are commonly absent from libGL's dynamic symbol table. The
         * redirected context is an EGL OpenGL context, so ask EGL for those functions. */
        addr = (void *)eglGetProcAddress(name);
    }
    return addr;
}

void (*glXGetProcAddressARB(const GLubyte *s))(void) {
    return (void (*)(void))glx_get_proc_address_impl(s);
}

void (*glXGetProcAddress(const GLubyte *s))(void) {
    return (void (*)(void))glx_get_proc_address_impl(s);
}

void resolve_dlsym(void){
    if (!real_dlsym) {
        real_dlsym = dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.2.5");
        if (!real_dlsym) {
            fprintf(stderr, "[X] Failed to resolve real real_dlsym(..)\n");
            assert(0);      
            return;
        }
    }
}

/* Under the VM, key, button and pointer events come through the shared ring in the comm page by
 * default. EGL_SHARED_INPUT=0 falls back to receiving them over the application's X connection. */
static bool shared_input_requested(void) {
    static int enabled = -1;
    if (enabled < 0) {
        const char *value = getenv("EGL_SHARED_INPUT");
        enabled = !(value && strcmp(value, "0") == 0);
    }
    /* Input redirection is a graphics use: it starts the session (see graphics_ensure_session()). */
    return in_gramine_vm && enabled && graphics_ensure_session();
}

static unsigned long redirected_core_input_mask(void) {
    return KeyPressMask | KeyReleaseMask | ButtonPressMask | ButtonReleaseMask |
           PointerMotionMask | PointerMotionHintMask | ButtonMotionMask |
           Button1MotionMask | Button2MotionMask | Button3MotionMask |
           Button4MotionMask | Button5MotionMask;
}

static sg_input_ring_t *guest_input_ring(void) {
    return (sg_input_ring_t *)(uintptr_t)(COMM_ADDR + SG_INPUT_RING_OFFSET);
}

static sg_input_latency_t *guest_input_latency(void) {
    return (sg_input_latency_t *)(uintptr_t)(COMM_ADDR + SG_INPUT_LATENCY_OFFSET);
}

static uint64_t shared_input_read_tsc(void) {
    uint32_t low;
    uint32_t high;

    __asm__ volatile("lfence; rdtscp; lfence"
                     : "=a"(low), "=d"(high) :: "rcx", "memory");
    return ((uint64_t)high << 32) | low;
}

static uint32_t delivered_click_sequence;

static void mark_shared_input_delivered(const sg_input_event_t *input) {
    if (input->type != ButtonPress || input->detail != Button1)
        return;

    sg_input_latency_t *latency = guest_input_latency();
    if (__atomic_load_n(&latency->magic, __ATOMIC_ACQUIRE) !=
            SG_INPUT_LATENCY_MAGIC ||
            __atomic_load_n(&latency->enabled, __ATOMIC_ACQUIRE) != 1)
        return;

    uint32_t sequence = ++delivered_click_sequence;
    __atomic_store_n(&latency->delivery_tsc, shared_input_read_tsc(),
                     __ATOMIC_RELAXED);
    __atomic_store_n(&latency->delivered_sequence, sequence, __ATOMIC_RELEASE);
}

static Window shared_input_window;

static void publish_shared_input_mask(Window window, unsigned long mask) {
    if (!shared_input_requested() || window == None)
        return;

    unsigned long input_mask = mask & redirected_core_input_mask();
    if (input_mask)
        shared_input_window = window;
    else if (window != shared_input_window)
        return;

    long offset = acquire_libos_lock();
    comm_page_t *c = comm_page(offset);
    c->p1 = window;
    c->p2 = input_mask;
    c->req_bit = UPDATE_INPUT_MASK;
    comm_sync_notify(c);
    relinquish_libos_lock(offset);
}

static void fill_xevent(Display *display, const sg_input_event_t *input, XEvent *event) {
    memset(event, 0, sizeof(*event));
    event->xany.type = input->type;
    event->xany.display = display;
    event->xany.window = input->window;
    event->xany.send_event = False;

    if (input->type == KeyPress || input->type == KeyRelease) {
        event->xkey.root = input->root;
        event->xkey.subwindow = input->child;
        event->xkey.time = input->time;
        event->xkey.x = input->event_x;
        event->xkey.y = input->event_y;
        event->xkey.x_root = input->root_x;
        event->xkey.y_root = input->root_y;
        event->xkey.state = input->state;
        event->xkey.keycode = input->detail;
        event->xkey.same_screen = input->same_screen;
    } else if (input->type == ButtonPress || input->type == ButtonRelease) {
        event->xbutton.root = input->root;
        event->xbutton.subwindow = input->child;
        event->xbutton.time = input->time;
        event->xbutton.x = input->event_x;
        event->xbutton.y = input->event_y;
        event->xbutton.x_root = input->root_x;
        event->xbutton.y_root = input->root_y;
        event->xbutton.state = input->state;
        event->xbutton.button = input->detail;
        event->xbutton.same_screen = input->same_screen;
    } else if (input->type == MotionNotify) {
        event->xmotion.root = input->root;
        event->xmotion.subwindow = input->child;
        event->xmotion.time = input->time;
        event->xmotion.x = input->event_x;
        event->xmotion.y = input->event_y;
        event->xmotion.x_root = input->root_x;
        event->xmotion.y_root = input->root_y;
        event->xmotion.state = input->state;
        event->xmotion.is_hint = NotifyNormal;
        event->xmotion.same_screen = input->same_screen;
    }
}

static uint32_t consumed_motion_serial;

static bool read_shared_motion(sg_input_ring_t *ring, sg_input_event_t *input,
                               uint32_t *event_serial) {
    for (int attempt = 0; attempt < 3; attempt++) {
        uint32_t serial_before = __atomic_load_n(&ring->motion_serial, __ATOMIC_ACQUIRE);
        uint32_t seq_before = __atomic_load_n(&ring->motion_seq, __ATOMIC_ACQUIRE);
        if ((seq_before & 1) || seq_before == 0)
            continue;

        *input = ring->motion;
        uint32_t seq_after = __atomic_load_n(&ring->motion_seq, __ATOMIC_ACQUIRE);
        uint32_t serial_after = __atomic_load_n(&ring->motion_serial, __ATOMIC_ACQUIRE);
        if (seq_before == seq_after && !(seq_after & 1) &&
                serial_before == serial_after) {
            if (event_serial)
                *event_serial = serial_after;
            return true;
        }
    }
    return false;
}

static bool peek_shared_input(Display *display, XEvent *event, bool consume) {
    if (!shared_input_requested())
        return false;

    sg_input_ring_t *ring = guest_input_ring();
    if (__atomic_load_n(&ring->magic, __ATOMIC_ACQUIRE) != SG_INPUT_RING_MAGIC ||
            __atomic_load_n(&ring->enabled, __ATOMIC_ACQUIRE) == 0)
        return false;

    uint32_t head = __atomic_load_n(&ring->head, __ATOMIC_RELAXED);
    uint32_t tail = __atomic_load_n(&ring->tail, __ATOMIC_ACQUIRE);
    if (head != tail) {
        sg_input_event_t input = ring->events[head % SG_INPUT_RING_CAPACITY];
        fill_xevent(display, &input, event);
        if (consume) {
            __atomic_store_n(&ring->head, head + 1, __ATOMIC_RELEASE);
            mark_shared_input_delivered(&input);
        }
        return true;
    }

    uint32_t serial = __atomic_load_n(&ring->motion_serial, __ATOMIC_ACQUIRE);
    if (serial == consumed_motion_serial)
        return false;

    sg_input_event_t input;
    if (!read_shared_motion(ring, &input, &serial) || serial == consumed_motion_serial)
        return false;
    fill_xevent(display, &input, event);
    if (consume)
        consumed_motion_serial = serial;
    return true;
}

static int shared_input_pending(void) {
    if (!shared_input_requested())
        return 0;

    sg_input_ring_t *ring = guest_input_ring();
    if (__atomic_load_n(&ring->magic, __ATOMIC_ACQUIRE) != SG_INPUT_RING_MAGIC ||
            __atomic_load_n(&ring->enabled, __ATOMIC_ACQUIRE) == 0)
        return 0;

    uint32_t head = __atomic_load_n(&ring->head, __ATOMIC_RELAXED);
    uint32_t tail = __atomic_load_n(&ring->tail, __ATOMIC_ACQUIRE);
    uint32_t motion = __atomic_load_n(&ring->motion_serial, __ATOMIC_ACQUIRE);
    return (int)(tail - head) + (motion != consumed_motion_serial);
}

Bool XQueryPointer(Display *display, Window window, Window *root_return,
                   Window *child_return, int *root_x_return, int *root_y_return,
                   int *win_x_return, int *win_y_return, unsigned int *mask_return) {
    typedef Bool (*x_query_pointer_t)(Display *, Window, Window *, Window *, int *, int *,
                                      int *, int *, unsigned int *);
    static x_query_pointer_t real_XQueryPointer;

    if (shared_input_requested()) {
        sg_input_ring_t *ring = guest_input_ring();
        sg_input_event_t pointer;
        if (__atomic_load_n(&ring->magic, __ATOMIC_ACQUIRE) == SG_INPUT_RING_MAGIC &&
                __atomic_load_n(&ring->enabled, __ATOMIC_ACQUIRE) != 0 &&
                read_shared_motion(ring, &pointer, NULL) && pointer.window == window) {
            if (root_return)
                *root_return = pointer.root;
            if (child_return)
                *child_return = pointer.child;
            if (root_x_return)
                *root_x_return = pointer.root_x;
            if (root_y_return)
                *root_y_return = pointer.root_y;
            if (win_x_return)
                *win_x_return = pointer.event_x;
            if (win_y_return)
                *win_y_return = pointer.event_y;
            if (mask_return)
                *mask_return = pointer.state;
            return pointer.same_screen ? True : False;
        }
    }

    resolve_dlsym();
    if (!real_XQueryPointer)
        real_XQueryPointer = (x_query_pointer_t)real_dlsym(RTLD_NEXT, "XQueryPointer");
    return real_XQueryPointer ?
        real_XQueryPointer(display, window, root_return, child_return, root_x_return,
                           root_y_return, win_x_return, win_y_return, mask_return) : False;
}

Window XCreateWindow(Display *display, Window parent, int x, int y,
                     unsigned int width, unsigned int height, unsigned int border_width,
                     int depth, unsigned int class, Visual *visual, unsigned long value_mask,
                     XSetWindowAttributes *attributes) {
    typedef Window (*x_create_window_t)(Display *, Window, int, int, unsigned int, unsigned int,
                                         unsigned int, int, unsigned int, Visual *, unsigned long,
                                         XSetWindowAttributes *);
    static x_create_window_t real_XCreateWindow;
    resolve_dlsym();
    if (!real_XCreateWindow)
        real_XCreateWindow = (x_create_window_t)real_dlsym(RTLD_NEXT, "XCreateWindow");

    XSetWindowAttributes filtered;
    unsigned long input_mask = 0;
    if (shared_input_requested() && attributes && (value_mask & CWEventMask)) {
        filtered = *attributes;
        input_mask = filtered.event_mask & redirected_core_input_mask();
        filtered.event_mask &= ~redirected_core_input_mask();
        attributes = &filtered;
    }

    Window window = real_XCreateWindow ?
        real_XCreateWindow(display, parent, x, y, width, height, border_width, depth, class,
                           visual, value_mask, attributes) : None;
    if (window != None && input_mask) {
        set_guest_app_window((xcb_window_t)window);
        publish_shared_input_mask(window, input_mask);
    }
    return window;
}

int XSelectInput(Display *display, Window window, long event_mask) {
    typedef int (*x_select_input_t)(Display *, Window, long);
    static x_select_input_t real_XSelectInput;
    resolve_dlsym();
    if (!real_XSelectInput)
        real_XSelectInput = (x_select_input_t)real_dlsym(RTLD_NEXT, "XSelectInput");

    long filtered = event_mask;
    if (shared_input_requested())
        filtered &= ~(long)redirected_core_input_mask();
    int ret = real_XSelectInput ? real_XSelectInput(display, window, filtered) : 0;
    publish_shared_input_mask(window, event_mask);
    return ret;
}

int XChangeWindowAttributes(Display *display, Window window, unsigned long value_mask,
                            XSetWindowAttributes *attributes) {
    typedef int (*x_change_attributes_t)(Display *, Window, unsigned long,
                                          XSetWindowAttributes *);
    static x_change_attributes_t real_XChangeWindowAttributes;
    resolve_dlsym();
    if (!real_XChangeWindowAttributes)
        real_XChangeWindowAttributes =
            (x_change_attributes_t)real_dlsym(RTLD_NEXT, "XChangeWindowAttributes");

    XSetWindowAttributes filtered;
    unsigned long input_mask = 0;
    if (shared_input_requested() && attributes && (value_mask & CWEventMask)) {
        filtered = *attributes;
        input_mask = filtered.event_mask & redirected_core_input_mask();
        filtered.event_mask &= ~redirected_core_input_mask();
        attributes = &filtered;
    }
    int ret = real_XChangeWindowAttributes ?
        real_XChangeWindowAttributes(display, window, value_mask, attributes) : 0;
    if (value_mask & CWEventMask)
        publish_shared_input_mask(window, input_mask);
    return ret;
}

int XPending(Display *display) {
    typedef int (*x_pending_t)(Display *);
    static x_pending_t real_XPending;
    resolve_dlsym();
    if (!real_XPending)
        real_XPending = (x_pending_t)real_dlsym(RTLD_NEXT, "XPending");
    return shared_input_pending() + (real_XPending ? real_XPending(display) : 0);
}

int XNextEvent(Display *display, XEvent *event) {
    typedef int (*x_next_event_t)(Display *, XEvent *);
    static x_next_event_t real_XNextEvent;
    if (peek_shared_input(display, event, /*consume=*/true))
        return 0;
    resolve_dlsym();
    if (!real_XNextEvent)
        real_XNextEvent = (x_next_event_t)real_dlsym(RTLD_NEXT, "XNextEvent");
    return real_XNextEvent ? real_XNextEvent(display, event) : 0;
}

/* SDL's X11 backend only calls XNextEvent() when XEventsQueued() reports queued events or the
 * connection is readable, so shared-ring events have to be counted here as well. */
int XEventsQueued(Display *display, int mode) {
    typedef int (*x_events_queued_t)(Display *, int);
    static x_events_queued_t real_XEventsQueued;
    resolve_dlsym();
    if (!real_XEventsQueued)
        real_XEventsQueued = (x_events_queued_t)real_dlsym(RTLD_NEXT, "XEventsQueued");
    return shared_input_pending() + (real_XEventsQueued ? real_XEventsQueued(display, mode) : 0);
}

/* SDL detects key auto-repeat by scanning the queue for the KeyPress that pairs with a KeyRelease.
 * Offer the predicate the discrete events waiting in the shared ring first; without this every
 * auto-repeat would reach the application as a real release followed by a press. An event the
 * predicate accepts is consumed only when it is the oldest one, to keep the ring ordered. */
Bool XCheckIfEvent(Display *display, XEvent *event,
                   Bool (*predicate)(Display *, XEvent *, XPointer), XPointer arg) {
    typedef Bool (*x_check_if_event_t)(Display *, XEvent *,
                                       Bool (*)(Display *, XEvent *, XPointer), XPointer);
    static x_check_if_event_t real_XCheckIfEvent;

    if (shared_input_requested() && predicate) {
        sg_input_ring_t *ring = guest_input_ring();
        if (__atomic_load_n(&ring->magic, __ATOMIC_ACQUIRE) == SG_INPUT_RING_MAGIC &&
                __atomic_load_n(&ring->enabled, __ATOMIC_ACQUIRE) != 0) {
            uint32_t head = __atomic_load_n(&ring->head, __ATOMIC_RELAXED);
            uint32_t tail = __atomic_load_n(&ring->tail, __ATOMIC_ACQUIRE);
            for (uint32_t i = head; i != tail; i++) {
                sg_input_event_t input = ring->events[i % SG_INPUT_RING_CAPACITY];
                XEvent queued;
                fill_xevent(display, &input, &queued);
                if (predicate(display, &queued, arg) && i == head) {
                    *event = queued;
                    __atomic_store_n(&ring->head, head + 1, __ATOMIC_RELEASE);
                    mark_shared_input_delivered(&input);
                    return True;
                }
            }
        }
    }

    resolve_dlsym();
    if (!real_XCheckIfEvent)
        real_XCheckIfEvent = (x_check_if_event_t)real_dlsym(RTLD_NEXT, "XCheckIfEvent");
    return real_XCheckIfEvent ? real_XCheckIfEvent(display, event, predicate, arg) : False;
}

int XPeekEvent(Display *display, XEvent *event) {
    typedef int (*x_peek_event_t)(Display *, XEvent *);
    static x_peek_event_t real_XPeekEvent;
    if (peek_shared_input(display, event, /*consume=*/false))
        return 0;
    resolve_dlsym();
    if (!real_XPeekEvent)
        real_XPeekEvent = (x_peek_event_t)real_dlsym(RTLD_NEXT, "XPeekEvent");
    return real_XPeekEvent ? real_XPeekEvent(display, event) : 0;
}

void SDL_SetWindowIcon(SDL_Window *window, SDL_Surface *icon) {
    typedef void (*sdl_set_window_icon_t)(SDL_Window *, SDL_Surface *);
    static sdl_set_window_icon_t real_SDL_SetWindowIcon;

    if (in_gramine_vm)
        return;

    resolve_dlsym();
    if (!real_SDL_SetWindowIcon)
        real_SDL_SetWindowIcon = (sdl_set_window_icon_t)real_dlsym(RTLD_NEXT,
                                                                   "SDL_SetWindowIcon");
    if (real_SDL_SetWindowIcon)
        real_SDL_SetWindowIcon(window, icon);
}

static bool sdl_logging_enabled(void) {
    const char* enabled = getenv("EGL_SDL_LOG");
    return enabled && strcmp(enabled, "1") == 0;
}

static void* resolve_sdl_symbol(const char* name) {
    resolve_dlsym();
    void* symbol = real_dlsym(RTLD_NEXT, name);
    if (symbol)
        return symbol;

    void* handle = dlopen("libSDL2-2.0.so.0", RTLD_LAZY | RTLD_NOLOAD);
    if (!handle)
        handle = dlopen("libSDL2-2.0.so.0", RTLD_LAZY | RTLD_LOCAL);
    return handle ? real_dlsym(handle, name) : NULL;
}

static const char* sdl_error(void) {
    typedef const char* (*sdl_get_error_t)(void);
    sdl_get_error_t real = (sdl_get_error_t)resolve_sdl_symbol("SDL_GetError");
    return real ? real() : "SDL_GetError unavailable";
}

Bool XFilterEvent(XEvent* event, Window window) {
    typedef Bool (*x_filter_event_t)(XEvent*, Window);
    static x_filter_event_t real_XFilterEvent;
    resolve_dlsym();
    if (!real_XFilterEvent)
        real_XFilterEvent =
            (x_filter_event_t)real_dlsym(RTLD_NEXT, "XFilterEvent");

    /* wlroots/Xwayland may transition from PointerRoot to explicit focus on
     * the same application window after re-entry. That produces a trailing
     * FocusOut/NotifyPointer even though the top-level remains active. Older
     * direct-Xlib game loops (notably Irrlicht) mistake it for real focus loss
     * and permanently stop accepting input. Real focus changes use another
     * detail (NotifyNonlinear in our observed switch-away path) and continue
     * through the normal XIM filter. */
    if (in_gramine_vm && event && event->type == FocusOut &&
            event->xfocus.detail == NotifyPointer)
        return True;

    return real_XFilterEvent ? real_XFilterEvent(event, window) : False;
}

int SDL_Init(Uint32 flags) {
    typedef int (*sdl_init_t)(Uint32);
    sdl_init_t real = (sdl_init_t)resolve_sdl_symbol("SDL_Init");
    if (flags & SDL_INIT_VIDEO)
        graphics_ensure_session();
    if (sdl_logging_enabled())
        fprintf(stderr, "[sdl] SDL_Init(flags=0x%x) begin\n", flags);
    int ret = real ? real(flags) : -1;
    if (sdl_logging_enabled())
        fprintf(stderr, "[sdl] SDL_Init end: %d (%s)\n", ret, sdl_error());
    return ret;
}

int SDL_InitSubSystem(Uint32 flags) {
    typedef int (*sdl_init_subsystem_t)(Uint32);
    sdl_init_subsystem_t real =
        (sdl_init_subsystem_t)resolve_sdl_symbol("SDL_InitSubSystem");
    if (flags & SDL_INIT_VIDEO)
        graphics_ensure_session();
    if (sdl_logging_enabled())
        fprintf(stderr, "[sdl] SDL_InitSubSystem(flags=0x%x) begin\n", flags);
    int ret = real ? real(flags) : -1;
    if (sdl_logging_enabled())
        fprintf(stderr, "[sdl] SDL_InitSubSystem end: %d (%s)\n", ret, sdl_error());
    return ret;
}

int SDL_VideoInit(const char* driver_name) {
    typedef int (*sdl_video_init_t)(const char*);
    sdl_video_init_t real = (sdl_video_init_t)resolve_sdl_symbol("SDL_VideoInit");
    graphics_ensure_session();
    if (sdl_logging_enabled())
        fprintf(stderr, "[sdl] SDL_VideoInit(driver=%s) begin\n",
                driver_name ? driver_name : "(default)");
    int ret = real ? real(driver_name) : -1;
    if (sdl_logging_enabled())
        fprintf(stderr, "[sdl] SDL_VideoInit end: %d (%s)\n", ret, sdl_error());
    return ret;
}

SDL_Window* SDL_CreateWindow(const char* title, int x, int y, int width, int height, Uint32 flags) {
    typedef SDL_Window* (*sdl_create_window_t)(const char*, int, int, int, int, Uint32);
    typedef SDL_bool (*sdl_get_window_wm_info_t)(SDL_Window*, SDL_SysWMinfo*);
    sdl_create_window_t real =
        (sdl_create_window_t)resolve_sdl_symbol("SDL_CreateWindow");
    if (sdl_logging_enabled())
        fprintf(stderr, "[sdl] SDL_CreateWindow(%s, %dx%d, flags=0x%x) begin\n",
                title ? title : "(null)", width, height, flags);
    set_guest_app_window_creation_in_progress(true);
    if (in_gramine_vm) {
        graphics_ensure_session();
        /* SDL's X11 backend briefly makes a private 32x32 GLX drawable current
         * while constructing the requested window. Allocate redirect buffers
         * at the requested size from the outset; rebuilding the complete
         * GBM/EGL/FBO set during this bootstrap path can race Mesa startup. */
        if (width > 0 && height > 0) {
            win_width = width;
            win_height = height;
        }
        /* The listener must know the requested dimensions before the internal
         * glXMakeCurrent imports any DMA-BUFs. The creation flag deliberately
         * leaves the presentation window unmapped until SDL exposes its
         * durable native parent below. */
        update_vm_window(XCB_NONE);
    }
    SDL_Window* ret = real ? real(title, x, y, width, height, flags) : NULL;
    xcb_window_t native_window = XCB_NONE;
    if (ret) {
        sdl_get_window_wm_info_t get_wm_info =
            (sdl_get_window_wm_info_t)resolve_sdl_symbol("SDL_GetWindowWMInfo");
        SDL_SysWMinfo info;
        SDL_VERSION(&info.version);
        if (get_wm_info && get_wm_info(ret, &info) == SDL_TRUE &&
                info.subsystem == SDL_SYSWM_X11) {
            native_window = (xcb_window_t)info.info.x11.window;
            /* Order SDL's Xlib window creation before presentation from the
             * redirector's independent XCB connection. */
            XSync(info.info.x11.display, False);
            set_guest_app_window(native_window);
            if (sdl_logging_enabled())
                fprintf(stderr, "[sdl] native X11 window: 0x%lx\n",
                        info.info.x11.window);
        }
    }
    set_guest_app_window_creation_in_progress(false);
    if (in_gramine_vm) {
        /* Publish the durable parent and its requested size immediately. Some
         * SDL applications begin rendering without another intercepted
         * glXMakeCurrent/SwapBuffers transition that would otherwise deliver
         * this update, leaving the listener child at its default 300x300. */
        if (native_window != XCB_NONE)
            update_vm_window(native_window);
    } else if (native_window != XCB_NONE) {
        use_native_presentation_window(native_window);
    }
    if (sdl_logging_enabled())
        fprintf(stderr, "[sdl] SDL_CreateWindow end: %p (%s)\n", (void*)ret, sdl_error());
    return ret;
}

SDL_GLContext SDL_GL_CreateContext(SDL_Window* window) {
    typedef SDL_GLContext (*sdl_gl_create_context_t)(SDL_Window*);
    sdl_gl_create_context_t real =
        (sdl_gl_create_context_t)resolve_sdl_symbol("SDL_GL_CreateContext");
    if (sdl_logging_enabled())
        fprintf(stderr, "[sdl] SDL_GL_CreateContext(window=%p) begin\n", (void*)window);
    SDL_GLContext ret = real ? real(window) : NULL;
    if (sdl_logging_enabled())
        fprintf(stderr, "[sdl] SDL_GL_CreateContext end: %p (%s)\n", ret, sdl_error());
    return ret;
}

int SDL_GL_LoadLibrary(const char* path) {
    typedef int (*sdl_gl_load_library_t)(const char*);
    sdl_gl_load_library_t real =
        (sdl_gl_load_library_t)resolve_sdl_symbol("SDL_GL_LoadLibrary");
    if (sdl_logging_enabled())
        fprintf(stderr, "[sdl] SDL_GL_LoadLibrary(%s) begin\n", path ? path : "(default)");
    int ret = real ? real(path) : -1;
    if (sdl_logging_enabled())
        fprintf(stderr, "[sdl] SDL_GL_LoadLibrary end: %d (%s)\n", ret, sdl_error());
    return ret;
}

int SDL_GL_SetAttribute(SDL_GLattr attr, int value) {
    typedef int (*sdl_gl_set_attribute_t)(SDL_GLattr, int);
    sdl_gl_set_attribute_t real =
        (sdl_gl_set_attribute_t)resolve_sdl_symbol("SDL_GL_SetAttribute");
    if (sdl_logging_enabled())
        fprintf(stderr, "[sdl] SDL_GL_SetAttribute(attr=%d, value=%d) begin\n", attr, value);
    int ret = real ? real(attr, value) : -1;
    if (sdl_logging_enabled())
        fprintf(stderr, "[sdl] SDL_GL_SetAttribute end: %d (%s)\n", ret, sdl_error());
    return ret;
}

const char* SDL_GetCurrentVideoDriver(void) {
    typedef const char* (*sdl_get_current_video_driver_t)(void);
    sdl_get_current_video_driver_t real =
        (sdl_get_current_video_driver_t)resolve_sdl_symbol("SDL_GetCurrentVideoDriver");
    const char* ret = real ? real() : NULL;
    if (sdl_logging_enabled())
        fprintf(stderr, "[sdl] SDL_GetCurrentVideoDriver -> %s\n", ret ? ret : "(null)");
    return ret;
}

int SDL_GetNumVideoDisplays(void) {
    typedef int (*sdl_get_num_video_displays_t)(void);
    sdl_get_num_video_displays_t real =
        (sdl_get_num_video_displays_t)resolve_sdl_symbol("SDL_GetNumVideoDisplays");
    if (sdl_logging_enabled())
        fprintf(stderr, "[sdl] SDL_GetNumVideoDisplays begin\n");
    int ret = real ? real() : -1;
    if (sdl_logging_enabled())
        fprintf(stderr, "[sdl] SDL_GetNumVideoDisplays end: %d (%s)\n", ret, sdl_error());
    return ret;
}

int SDL_GetDesktopDisplayMode(int display_index, SDL_DisplayMode* mode) {
    typedef int (*sdl_get_desktop_display_mode_t)(int, SDL_DisplayMode*);
    sdl_get_desktop_display_mode_t real =
        (sdl_get_desktop_display_mode_t)resolve_sdl_symbol("SDL_GetDesktopDisplayMode");
    if (sdl_logging_enabled())
        fprintf(stderr, "[sdl] SDL_GetDesktopDisplayMode(display=%d) begin\n", display_index);
    int ret = real ? real(display_index, mode) : -1;
    if (sdl_logging_enabled())
        fprintf(stderr, "[sdl] SDL_GetDesktopDisplayMode end: %d (%s)\n", ret, sdl_error());
    return ret;
}

int SDL_GetDisplayBounds(int display_index, SDL_Rect* rect) {
    typedef int (*sdl_get_display_bounds_t)(int, SDL_Rect*);
    sdl_get_display_bounds_t real =
        (sdl_get_display_bounds_t)resolve_sdl_symbol("SDL_GetDisplayBounds");
    if (sdl_logging_enabled())
        fprintf(stderr, "[sdl] SDL_GetDisplayBounds(display=%d) begin\n", display_index);
    int ret = real ? real(display_index, rect) : -1;
    if (sdl_logging_enabled())
        fprintf(stderr, "[sdl] SDL_GetDisplayBounds end: %d (%s)\n", ret, sdl_error());
    return ret;
}

Uint32 SDL_WasInit(Uint32 flags) {
    typedef Uint32 (*sdl_was_init_t)(Uint32);
    sdl_was_init_t real = (sdl_was_init_t)resolve_sdl_symbol("SDL_WasInit");
    Uint32 ret = real ? real(flags) : 0;
    if (sdl_logging_enabled())
        fprintf(stderr, "[sdl] SDL_WasInit(flags=0x%x) -> 0x%x\n", flags, ret);
    return ret;
}

int SDL_ShowSimpleMessageBox(Uint32 flags, const char* title, const char* message,
                             SDL_Window* window) {
    if (in_gramine_vm) {
        fprintf(stderr, "[sdl] suppressing modal message box: %s: %s\n",
                title ? title : "(untitled)", message ? message : "(no message)");
        return 0;
    }

    typedef int (*sdl_show_simple_message_box_t)(Uint32, const char*, const char*, SDL_Window*);
    sdl_show_simple_message_box_t real =
        (sdl_show_simple_message_box_t)resolve_sdl_symbol("SDL_ShowSimpleMessageBox");
    return real ? real(flags, title, message, window) : -1;
}

static void* resolve_egl_redirect_symbol(const char* symbol) {
    static void* self_handle;
    if (!self_handle)
        self_handle = dlopen("/libgraphics.so", RTLD_LAZY | RTLD_NOLOAD);
    return self_handle ? real_dlsym(self_handle, symbol) : NULL;
}

pid_t fork(void) {
    typedef pid_t (*fork_t)(void);
    resolve_dlsym();
    fork_t real = (fork_t)real_dlsym(RTLD_NEXT, "fork");

    const char* logging = getenv("EGL_PROCESS_LOG");
    if (logging && strcmp(logging, "1") == 0) {
        void* caller = __builtin_return_address(0);
        Dl_info info = {0};
        dladdr(caller, &info);
        fprintf(stderr, "[process] fork called from %s (%s+0x%lx)\n",
                info.dli_fname ? info.dli_fname : "(unknown)",
                info.dli_sname ? info.dli_sname : "(unknown)",
                info.dli_saddr ? (unsigned long)((uintptr_t)caller - (uintptr_t)info.dli_saddr) : 0);
    }

    return real ? real() : -1;
}

/* dlsym(RTLD_NEXT, symbol) on behalf of the object containing `caller`. glibc resolves RTLD_NEXT
 * relative to the object that calls dlsym, which is this library once dlsym is interposed below,
 * so passing RTLD_NEXT through would search after libgraphics.so instead of after the caller (a
 * library preloaded after this one then finds its own function: endless recursion). Walk the
 * link maps after the caller's instead and return the first definition that belongs to one of
 * them. */
static void *dlsym_next_after(void *caller, const char *symbol) {
    Dl_info info;
    struct link_map *caller_map = NULL;
    if (!dladdr1(caller, &info, (void **)&caller_map, RTLD_DL_LINKMAP) || !caller_map)
        return real_dlsym(RTLD_NEXT, symbol);
    for (struct link_map *m = caller_map->l_next; m; m = m->l_next) {
        if (!m->l_name || !m->l_name[0])
            continue;
        void *h = dlopen(m->l_name, RTLD_LAZY | RTLD_NOLOAD);
        if (!h)
            continue;
        void *sym = real_dlsym(h, symbol);
        dlclose(h);
        struct link_map *sym_map = NULL;
        if (sym && dladdr1(sym, &info, (void **)&sym_map, RTLD_DL_LINKMAP) && sym_map == m)
            return sym;
    }
    return NULL;
}

void *dlsym(void *handle, const char *symbol) {
    if (!real_dlsym) {
        real_dlsym = dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.2.5");
        if (!real_dlsym) {
            fprintf(stderr, "[X] Failed to resolve real real_dlsym(..)\n");
            assert(0);      
            return NULL;
        }
    }

    void *sym = handle == RTLD_NEXT ? dlsym_next_after(__builtin_return_address(0), symbol)
                                    : real_dlsym(handle, symbol);
    if (dlsym_logging_enabled)
        fprintf(stderr, "[dlsym] %s -> %p\n", symbol, sym);
    if (strcmp(symbol, "glXSwapBuffers") == 0)
        return resolve_egl_redirect_symbol("glXSwapBuffers") ?: (void*)glXSwapBuffers;
    if (strcmp(symbol, "glXMakeCurrent") == 0)
        return resolve_egl_redirect_symbol("glXMakeCurrent") ?: (void*)glXMakeCurrent;
    if (strcmp(symbol, "glXGetProcAddress") == 0 ||
        strcmp(symbol, "glXGetProcAddressARB") == 0)
        return resolve_egl_redirect_symbol("glXGetProcAddress") ?: (void*)glXGetProcAddress;
    if (strcmp(symbol, "glXSwapIntervalEXT") == 0)
        return resolve_egl_redirect_symbol("glXSwapIntervalEXT") ?: (void*)glXSwapIntervalEXT;
    if (strcmp(symbol, "glXChooseVisual") == 0)
        return resolve_egl_redirect_symbol("glXChooseVisual") ?: (void*)glXChooseVisual;
    if (strcmp(symbol, "glXCreateContext") == 0)
        return resolve_egl_redirect_symbol("glXCreateContext") ?: (void*)glXCreateContext;
    if (strcmp(symbol, "glXQueryExtension") == 0)
        return resolve_egl_redirect_symbol("glXQueryExtension") ?: (void*)glXQueryExtension;
    if (strcmp(symbol, "glBindFramebuffer") == 0 ||
        strcmp(symbol, "glBindFramebufferEXT") == 0)
        return resolve_egl_redirect_symbol("glBindFramebuffer") ?: (void*)glBindFramebuffer;
    if (strcmp(symbol, "SDL_Init") == 0) return SDL_Init;
    if (strcmp(symbol, "SDL_InitSubSystem") == 0) return SDL_InitSubSystem;
    if (strcmp(symbol, "SDL_VideoInit") == 0) return SDL_VideoInit;
    if (strcmp(symbol, "SDL_CreateWindow") == 0) return SDL_CreateWindow;
    if (strcmp(symbol, "SDL_GL_CreateContext") == 0) return SDL_GL_CreateContext;
    if (strcmp(symbol, "SDL_SetWindowIcon") == 0) return SDL_SetWindowIcon;
    if (strcmp(symbol, "XQueryPointer") == 0) return XQueryPointer;

    return sym;
}

static const char* glximpl_name_to_string(int name) {
    switch (name) {
        case GLX_VENDOR:
            return "SharedGL";
        case GLX_VERSION:
            return glx_majmin_string;
        case GLX_EXTENSIONS:
            return glx_extensions;
    }
    return "?";
}

typedef GLXContext (*glXCreateContext_t)(Display *dpy,
                                         XVisualInfo *vis,
                                         GLXContext shareList,
                                         Bool direct);
static glXCreateContext_t real_glXCreateContext = NULL;
GLXContext glXCreateContext(Display* dpy, XVisualInfo* vis, GLXContext share_list, Bool direct) {
    graphics_ensure_session();
    if (glx_logging_enabled)
        fprintf(stderr, "[glx] glXCreateContext(visual=%p, share=%p, direct=%d)\n",
                (void*)vis, share_list, direct);
    #ifdef NATIVE_GLX
    if (!in_gramine_vm) {
        resolve_dlsym();
        real_glXCreateContext = (glXCreateContext_t) real_dlsym(RTLD_NEXT, "glXCreateContext");
        if (!real_glXCreateContext) {
            const char *err = dlerror();
            if (err) {
                fprintf(stderr, "[glxwrap] dlsym(RTLD_NEXT, \"glXCreateContext\") failed: %s\n", err);
            }
        }
        fprintf(stderr, "[glxwrap] Calling real glXCreateContext(..)\n");
        return real_glXCreateContext(dpy, vis, share_list, direct);
    }
    #endif
    return (GLXContext)1;
}

GLXContext glXGetCurrentContext(void) {
    #ifdef NATIVE_GLX
    if (!in_gramine_vm) {
        resolve_dlsym();
        static GLXContext (*real_glXGetCurrentContext)(void) = NULL;
        if (!real_glXGetCurrentContext)
            real_glXGetCurrentContext = real_dlsym(RTLD_NEXT, "glXGetCurrentContext");
        if (real_glXGetCurrentContext)
            return real_glXGetCurrentContext();
    }
    #endif
    return current_glx_context;
}

void glXDestroyContext(Display* dpy, GLXContext ctx) {
    #ifdef NATIVE_GLX
    if (!in_gramine_vm) {
        resolve_dlsym();
        static void (*real_glXDestroyContext)(Display*, GLXContext) = NULL;
        if (!real_glXDestroyContext)
            real_glXDestroyContext = real_dlsym(RTLD_NEXT, "glXDestroyContext");
        if (real_glXDestroyContext)
            real_glXDestroyContext(dpy, ctx);
        return;
    }
    #endif
}

Bool glXMakeContextCurrent(Display* dpy, GLXDrawable draw, GLXDrawable read, GLXContext ctx) {
    #ifdef NATIVE_GLX
    if (!in_gramine_vm) {
        resolve_dlsym();
        static Bool (*real_glXMakeContextCurrent)(Display*, GLXDrawable, GLXDrawable, GLXContext) = NULL;
        if (!real_glXMakeContextCurrent)
            real_glXMakeContextCurrent = real_dlsym(RTLD_NEXT, "glXMakeContextCurrent");
        if (real_glXMakeContextCurrent)
            return real_glXMakeContextCurrent(dpy, draw, read, ctx);
    }
    #endif
    (void)read;
    return glXMakeCurrent(dpy, draw, ctx);
}

Bool glXQueryExtension(Display* dpy, int* errorb, int* event) {
    if (glx_logging_enabled)
        fprintf(stderr, "[glx] glXQueryExtension\n");
    #ifdef NATIVE_GLX
    if (!in_gramine_vm) {
        resolve_dlsym();
        static Bool (*real_glXQueryExtension)(Display*, int*, int*) = NULL;
        if (!real_glXQueryExtension)
            real_glXQueryExtension = real_dlsym(RTLD_NEXT, "glXQueryExtension");
        if (real_glXQueryExtension)
            return real_glXQueryExtension(dpy, errorb, event);
    }
    #endif
    (void)dpy;
    if (errorb)
        *errorb = 0;
    if (event)
        *event = 0;
    return True;
}

void glXQueryDrawable(Display* dpy, GLXDrawable draw, int attribute, unsigned int* value) {
    #ifdef NATIVE_GLX
    if (!in_gramine_vm) {
        resolve_dlsym();
        static void (*real_glXQueryDrawable)(Display*, GLXDrawable, int, unsigned int*) = NULL;
        if (!real_glXQueryDrawable)
            real_glXQueryDrawable = real_dlsym(RTLD_NEXT, "glXQueryDrawable");
        if (real_glXQueryDrawable) {
            real_glXQueryDrawable(dpy, draw, attribute, value);
            return;
        }
    }
    #endif
    (void)dpy;
    (void)draw;
    if (!value)
        return;

    switch (attribute) {
        case GLX_WIDTH:
            *value = (unsigned int)win_width;
            break;
        case GLX_HEIGHT:
            *value = (unsigned int)win_height;
            break;
        case GLX_FBCONFIG_ID:
            *value = 1;
            break;
        case GLX_PRESERVED_CONTENTS:
            *value = False;
            break;
        default:
            *value = 0;
            break;
    }
}

const char* glXQueryExtensionsString(Display* dpy, int screen) {
    #ifdef NATIVE_GLX
    if (!in_gramine_vm) {
        resolve_dlsym();
        static const char* (*real_glXQueryExtensionsString)(Display*, int) = NULL;
        if (!real_glXQueryExtensionsString)
            real_glXQueryExtensionsString = real_dlsym(RTLD_NEXT, "glXQueryExtensionsString");
        if (real_glXQueryExtensionsString)
            return real_glXQueryExtensionsString(dpy, screen);
    }
    #endif
    return glx_extensions;
}

XVisualInfo *glXChooseVisual(Display *dpy, int screen, int *attrib_list)
{
    graphics_ensure_session();
    #ifdef NATIVE_GLX
    if (!in_gramine_vm) {
        resolve_dlsym();
        static XVisualInfo* (*real_glXChooseVisual)(Display*, int, int*) = NULL;
        if (!real_glXChooseVisual)
            real_glXChooseVisual = real_dlsym(RTLD_NEXT, "glXChooseVisual");
        if (real_glXChooseVisual)
            return real_glXChooseVisual(dpy, screen, attrib_list);
    }
    #endif 

    XVisualInfo *vinfo = malloc(sizeof(XVisualInfo));
    XMatchVisualInfo(dpy, XDefaultScreen(dpy), 24, TrueColor, vinfo);
    return vinfo;
}

GLXContext glXCreateNewContext(Display* dpy, GLXFBConfig config, int render_type,
                               GLXContext share_list, Bool direct) {
    #ifdef NATIVE_GLX
    if (!in_gramine_vm) {
        resolve_dlsym();
        static GLXContext (*real_glXCreateNewContext)(Display*, GLXFBConfig, int, GLXContext, Bool) = NULL;
        if (!real_glXCreateNewContext)
            real_glXCreateNewContext = real_dlsym(RTLD_NEXT, "glXCreateNewContext");
        if (real_glXCreateNewContext)
            return real_glXCreateNewContext(dpy, config, render_type, share_list, direct);
    }
    #endif 
    return (GLXContext)1;
}

Display* glXGetCurrentDisplay(void) {
    #ifdef NATIVE_GLX
    if (!in_gramine_vm) {
        resolve_dlsym();
        static Display* (*real_glXGetCurrentDisplay)(void) = NULL;
        if (!real_glXGetCurrentDisplay)
            real_glXGetCurrentDisplay = real_dlsym(RTLD_NEXT, "glXGetCurrentDisplay");
        if (real_glXGetCurrentDisplay)
            return real_glXGetCurrentDisplay();
    }
    #endif
    /* GLX returns the display belonging to the calling thread's current
     * context. Opening a new X connection here leaks the connection and, more
     * importantly in Gramine VM, can interleave several independent streams
     * through the vsock relay while SDL is still initializing. Irrlicht calls
     * this function repeatedly during driver setup. */
    return current_glx_display;
}

Bool glXQueryVersion(Display* dpy, int* maj, int* min) {
    if (glx_logging_enabled)
        fprintf(stderr, "[glx] glXQueryVersion\n");
    #ifdef NATIVE_GLX
    if (!in_gramine_vm) {
        resolve_dlsym();
        static Bool (*real_glXQueryVersion)(Display*, int*, int*) = NULL;
        if (!real_glXQueryVersion)
            real_glXQueryVersion = real_dlsym(RTLD_NEXT, "glXQueryVersion");
        if (real_glXQueryVersion)
            return real_glXQueryVersion(dpy, maj, min);
    }
    #endif 
    *maj = glx_major;
    *min = glx_minor;
    return True;
}

const char* glXGetClientString(Display* dpy, int name) {
    #ifdef NATIVE_GLX
    if (!in_gramine_vm) {
        resolve_dlsym();
        static const char* (*real_glXGetClientString)(Display*, int) = NULL;
        if (!real_glXGetClientString)
            real_glXGetClientString = real_dlsym(RTLD_NEXT, "glXGetClientString");
        if (real_glXGetClientString)
            return real_glXGetClientString(dpy, name);
    }
    #endif
    return glximpl_name_to_string(name);
}

GLXWindow glXCreateWindow(Display* dpy, GLXFBConfig config, Window win, const int* attrib_list) {
    #ifdef NATIVE_GLX
    if (!in_gramine_vm) {
        resolve_dlsym();
        static GLXWindow (*real_glXCreateWindow)(Display*, GLXFBConfig, Window, const int*) = NULL;
        if (!real_glXCreateWindow)
            real_glXCreateWindow = real_dlsym(RTLD_NEXT, "glXCreateWindow");
        if (real_glXCreateWindow)
            return real_glXCreateWindow(dpy, config, win, attrib_list);
    }
    #endif 
    (void)config; (void)attrib_list;
    return win;
}

void glXDestroyWindow(Display* dpy, GLXWindow win) {
    #ifdef NATIVE_GLX
    if (!in_gramine_vm) {
        resolve_dlsym();
        static void (*real_glXDestroyWindow)(Display*, GLXWindow) = NULL;
        if (!real_glXDestroyWindow)
            real_glXDestroyWindow = real_dlsym(RTLD_NEXT, "glXDestroyWindow");
        if (real_glXDestroyWindow)
            real_glXDestroyWindow(dpy, win);
        return;
    }
    #endif
}

int glXGetFBConfigAttrib(Display* dpy, GLXFBConfig config, int attribute, int* value) {
    static uint64_t call_count;
    call_count++;
    if (glx_logging_enabled && (call_count <= 20 || call_count % 1000 == 0))
        fprintf(stderr, "[glx] glXGetFBConfigAttrib(config=%p, attribute=%d), call=%lu\n",
                config, attribute, (unsigned long)call_count);
    #ifdef NATIVE_GLX
    if (!in_gramine_vm) {
        resolve_dlsym();
        static int (*real_glXGetFBConfigAttrib)(Display*, GLXFBConfig, int, int*) = NULL;
        if (!real_glXGetFBConfigAttrib)
            real_glXGetFBConfigAttrib = real_dlsym(RTLD_NEXT, "glXGetFBConfigAttrib");
        if (real_glXGetFBConfigAttrib)
            return real_glXGetFBConfigAttrib(dpy, config, attribute, value);
    }
    #endif 

    uintptr_t config_id = (uintptr_t)config;
    if (!value || config_id == 0 || config_id > (uintptr_t)n_valid_fb_configs) {
        fprintf(stderr, "glXGetFBConfigAttrib: invalid config %p (%d exist)\n",
                config, n_valid_fb_configs);
        return GLX_BAD_ATTRIBUTE;
    }
    unsigned int index = (unsigned int)config_id - 1;

    struct glx_fb_config fb_config = fb_configs[index];
    *value = 0;
    switch (attribute) {
        case GLX_FBCONFIG_ID:
            *value = (int)config_id;
            break;
        case GLX_RENDER_TYPE:
            *value = fb_config.render_type;  // GLX_RGBA_BIT;
            break;
        case GLX_VISUAL_ID:
            *value = XDefaultVisual(dpy, 0)->visualid;  // fb_config.visual_type;
            break;
        case GLX_BUFFER_SIZE:
            *value = 32;
            break;
        case GLX_SAMPLES:
            *value = 0;
            break;
        case GLX_SAMPLE_BUFFERS:
            *value = 1;
            break;
        case GLX_DRAWABLE_TYPE:
            *value = fb_config.drawable_type;  // GLX_WINDOW_BIT;
            break;
        case GLX_DOUBLEBUFFER:
            *value = fb_config.double_buffer;
            break;
        case GLX_RED_SIZE:
            *value = fb_config.red_size;
            break;
        case GLX_GREEN_SIZE:
            *value = fb_config.green_size;
            break;
        case GLX_BLUE_SIZE:
            *value = fb_config.blue_size;
            break;
        case GLX_ALPHA_SIZE:
            *value = fb_config.alpha_size;
            break;
        case GLX_STENCIL_SIZE:
            *value = 0;  // fb_config.stencil_size;
            break;
        case GLX_ACCUM_RED_SIZE:
            *value = fb_config.accum_red_size;
            break;
        case GLX_ACCUM_GREEN_SIZE:
            *value = fb_config.accum_green_size;
            break;
        case GLX_ACCUM_BLUE_SIZE:
            *value = fb_config.accum_blue_size;
            break;
        case GLX_ACCUM_ALPHA_SIZE:
            *value = fb_config.accum_alpha_size;  // 8;
            break;
        case GLX_DEPTH_SIZE:
            *value = 24;  // fb_config.depth_size; // 24;
            break;
        case GLX_FRAMEBUFFER_SRGB_CAPABLE_ARB:
            *value = 1;
            break;
        default:
            return GLX_BAD_ATTRIBUTE;
    }

    return Success;
}

GLXFBConfig* glXGetFBConfigs(Display* dpy, int screen, int* nelements) {
    if (glx_logging_enabled)
        fprintf(stderr, "[glx] glXGetFBConfigs(screen=%d)\n", screen);
    resolve_dlsym();
    #ifdef NATIVE_GLX
    if (!in_gramine_vm) {
        static GLXFBConfig* (*real_glXGetFBConfigs)(Display*, int, int*) = NULL;
        if (!real_glXGetFBConfigs)
            real_glXGetFBConfigs = real_dlsym(RTLD_NEXT, "glXGetFBConfigs");
        if (real_glXGetFBConfigs)
            return real_glXGetFBConfigs(dpy, screen, nelements);
    }
    #endif

    GLXFBConfig* fb_config = calloc(1, sizeof(GLXFBConfig) * n_valid_fb_configs);
    /* GLXFBConfig is opaque, but callers still treat NULL as failure. Use
     * one-based synthetic handles and translate them in GetFBConfigAttrib. */
    for (int i = 0; i < n_valid_fb_configs; i++)
        fb_config[i] = (GLXFBConfig)(uintptr_t)(i + 1);
    *nelements = n_valid_fb_configs;
    return fb_config;
}

XVisualInfo* glXGetVisualFromFBConfig(Display* dpy, GLXFBConfig config) {
    if (glx_logging_enabled)
        fprintf(stderr, "[glx] glXGetVisualFromFBConfig(config=%p)\n", config);
    #ifdef NATIVE_GLX
    if (!in_gramine_vm) {
        resolve_dlsym();
        static XVisualInfo* (*real_glXGetVisualFromFBConfig)(Display*, GLXFBConfig) = NULL;
        if (!real_glXGetVisualFromFBConfig)
            real_glXGetVisualFromFBConfig = real_dlsym(RTLD_NEXT, "glXGetVisualFromFBConfig");
        if (real_glXGetVisualFromFBConfig)
            return real_glXGetVisualFromFBConfig(dpy, config);
    }
    #endif

    XVisualInfo* vinfo = malloc(sizeof(XVisualInfo));
    XMatchVisualInfo(dpy, XDefaultScreen(dpy), 24, TrueColor, vinfo);
    return vinfo;
}

GLXContext glXCreateContextAttribsARB(Display* dpy, GLXFBConfig config, GLXContext share_context,
                                      Bool direct, const int* attrib_list) {
    #ifdef NATIVE_GLX
    if (!in_gramine_vm) {
        resolve_dlsym();
        static GLXContext (*real_glXCreateContextAttribsARB)(Display*, GLXFBConfig, GLXContext, Bool, const int*) = NULL;
        if (!real_glXCreateContextAttribsARB)
            real_glXCreateContextAttribsARB = real_dlsym(RTLD_NEXT, "glXCreateContextAttribsARB");
        if (real_glXCreateContextAttribsARB)
            return real_glXCreateContextAttribsARB(dpy, config, share_context, direct, attrib_list);
    }
    #endif 

    (void)config;
    (void)attrib_list;
    return glXCreateContext(dpy, NULL, share_context, direct);
}

int glXGetConfig(Display* dpy, XVisualInfo* visual, int attrib, int* value) {
    #ifdef NATIVE_GLX
    if (!in_gramine_vm) {
        resolve_dlsym();
        static int (*real_glXGetConfig)(Display*, XVisualInfo*, int, int*) = NULL;
        if (!real_glXGetConfig)
            real_glXGetConfig = real_dlsym(RTLD_NEXT, "glXGetConfig");
        if (real_glXGetConfig)
            return real_glXGetConfig(dpy, visual, attrib, value);
    }
    #endif
    (void)dpy;
    (void)visual;
    if (!value)
        return GLX_BAD_ATTRIBUTE;

    switch (attrib) {
        case GLX_USE_GL:
        case GLX_RGBA:
        case GLX_DOUBLEBUFFER:
            *value = True;
            break;
        case GLX_BUFFER_SIZE:
            *value = 32;
            break;
        case GLX_RED_SIZE:
        case GLX_GREEN_SIZE:
        case GLX_BLUE_SIZE:
        case GLX_ALPHA_SIZE:
        case GLX_STENCIL_SIZE:
            *value = 8;
            break;
        case GLX_DEPTH_SIZE:
            *value = 24;
            break;
        case GLX_LEVEL:
        case GLX_STEREO:
        case GLX_AUX_BUFFERS:
        case GLX_ACCUM_RED_SIZE:
        case GLX_ACCUM_GREEN_SIZE:
        case GLX_ACCUM_BLUE_SIZE:
        case GLX_ACCUM_ALPHA_SIZE:
            *value = 0;
            break;
        default:
            *value = 0;
            return GLX_BAD_ATTRIBUTE;
    }
    return Success;
}

GLXFBConfig* glXChooseFBConfig(Display* dpy, int screen, const int* attrib_list, int* nitems) {
    graphics_ensure_session();
    if (glx_logging_enabled)
        fprintf(stderr, "[glx] glXChooseFBConfig(screen=%d)\n", screen);
    #ifdef NATIVE_GLX
    if (!in_gramine_vm) {
        resolve_dlsym();
        static GLXFBConfig* (*real_glXChooseFBConfig)(Display*, int, const int*, int*) = NULL;
        if (!real_glXChooseFBConfig)
            real_glXChooseFBConfig = real_dlsym(RTLD_NEXT, "glXChooseFBConfig");
        if (real_glXChooseFBConfig)
            return real_glXChooseFBConfig(dpy, screen, attrib_list, nitems);
    }
    #endif 

    return glXGetFBConfigs(dpy, screen, nitems);
}

Bool glXIsDirect(Display* dpy, GLXContext ctx) {
    #ifdef NATIVE_GLX
    if (!in_gramine_vm) {
        resolve_dlsym();
        static Bool (*real_glXIsDirect)(Display*, GLXContext) = NULL;
        if (!real_glXIsDirect)
            real_glXIsDirect = real_dlsym(RTLD_NEXT, "glXIsDirect");
        if (real_glXIsDirect)
            return real_glXIsDirect(dpy, ctx);
    }
    #endif 
    return True;
}

const char* glXQueryServerString(Display* dpy, int screen, int name) {
    #ifdef NATIVE_GLX
    if (!in_gramine_vm) {
        resolve_dlsym();
        static const char* (*real_glXQueryServerString)(Display*, int, int) = NULL;
        if (!real_glXQueryServerString)
            real_glXQueryServerString = real_dlsym(RTLD_NEXT, "glXQueryServerString");
        if (real_glXQueryServerString)
            return real_glXQueryServerString(dpy, screen, name);
    }
    #endif 
    return glximpl_name_to_string(name);
}

void glXCopyContext(Display* dpy, GLXContext src, GLXContext dst, unsigned long mask) {
    #ifdef NATIVE_GLX
    if (!in_gramine_vm) {
        resolve_dlsym();
        static void (*real_glXCopyContext)(Display*, GLXContext, GLXContext, unsigned long) = NULL;
        if (!real_glXCopyContext)
            real_glXCopyContext = real_dlsym(RTLD_NEXT, "glXCopyContext");
        if (real_glXCopyContext)
            real_glXCopyContext(dpy, src, dst, mask);
    }
    #endif 
}

GLXPixmap glXCreateGLXPixmap(Display* dpy, XVisualInfo* visual, Pixmap pixmap) {
    #ifdef NATIVE_GLX
    if (!in_gramine_vm) {
        resolve_dlsym();
        static GLXPixmap (*real_glXCreateGLXPixmap)(Display*, XVisualInfo*, Pixmap) = NULL;
        if (!real_glXCreateGLXPixmap)
            real_glXCreateGLXPixmap = real_dlsym(RTLD_NEXT, "glXCreateGLXPixmap");
        if (real_glXCreateGLXPixmap)
            return real_glXCreateGLXPixmap(dpy, visual, pixmap);
    }
    #endif 
    return (GLXPixmap)1;
}

void glXDestroyGLXPixmap(Display* dpy, GLXPixmap pixmap) {
    #ifdef NATIVE_GLX
    if (!in_gramine_vm) {
        resolve_dlsym();
        static void (*real_glXDestroyGLXPixmap)(Display*, GLXPixmap) = NULL;
        if (!real_glXDestroyGLXPixmap)
            real_glXDestroyGLXPixmap = real_dlsym(RTLD_NEXT, "glXDestroyGLXPixmap");
        if (real_glXDestroyGLXPixmap)
            real_glXDestroyGLXPixmap(dpy, pixmap);
    }
    #endif 
}

GLXDrawable glXGetCurrentDrawable(void) {
    #ifdef NATIVE_GLX
    if (!in_gramine_vm) {
        resolve_dlsym();
        static GLXDrawable (*real_glXGetCurrentDrawable)(void) = NULL;
        if (!real_glXGetCurrentDrawable)
            real_glXGetCurrentDrawable = real_dlsym(RTLD_NEXT, "glXGetCurrentDrawable");
        if (real_glXGetCurrentDrawable)
            return real_glXGetCurrentDrawable();
    }
    #endif 
    return current_glx_drawable;
}

void glXUseXFont(Font font, int first, int count, int list) {
    #ifdef NATIVE_GLX
    if (!in_gramine_vm) {
        resolve_dlsym();
        static void (*real_glXUseXFont)(Font, int, int, int) = NULL;
        if (!real_glXUseXFont)
            real_glXUseXFont = real_dlsym(RTLD_NEXT, "glXUseXFont");
        if (real_glXUseXFont)
            real_glXUseXFont(font, first, count, list);
    }
    #endif
}

void glXWaitGL(void) {
    #ifdef NATIVE_GLX
    if (!in_gramine_vm) {
        resolve_dlsym();
        static void (*real_glXWaitGL)(void) = NULL;
        if (!real_glXWaitGL)
            real_glXWaitGL = real_dlsym(RTLD_NEXT, "glXWaitGL");
        if (real_glXWaitGL)
            real_glXWaitGL();
    }
    #endif 
}

void glXWaitX(void) {
    #ifdef NATIVE_GLX
    if (!in_gramine_vm) {
        resolve_dlsym();
        static void (*real_glXWaitX)(void) = NULL;
        if (!real_glXWaitX)
            real_glXWaitX = real_dlsym(RTLD_NEXT, "glXWaitX");
        if (real_glXWaitX)
            real_glXWaitX();
    }
    #endif 
}

int glXQueryContext(Display* dpy, GLXContext ctx, int attribute, int* value) {
    #ifdef NATIVE_GLX
    if (!in_gramine_vm) {
        resolve_dlsym();
        static int (*real_glXQueryContext)(Display*, GLXContext, int, int*) = NULL;
        if (!real_glXQueryContext)
            real_glXQueryContext = real_dlsym(RTLD_NEXT, "glXQueryContext");
        if (real_glXQueryContext)
            return real_glXQueryContext(dpy, ctx, attribute, value);
    }
    #endif 
    return Success;
}
