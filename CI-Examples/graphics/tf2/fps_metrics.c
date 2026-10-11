/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/* Preloaded frame-time collector: prints the average and 1% low frame rate of each interval. */

#define _GNU_SOURCE

#include <dlfcn.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <EGL/egl.h>
#include <GL/glx.h>

#define NS_PER_SECOND 1000000000ULL
#define DEFAULT_INTERVAL_SECONDS 60.0
#define DEFAULT_WARMUP_SECONDS 45.0

typedef void (*glXSwapBuffers_fn)(Display *, GLXDrawable);
typedef void (*SDL_GL_SwapWindow_fn)(void *);
typedef EGLBoolean (*eglSwapBuffers_fn)(EGLDisplay, EGLSurface);
typedef void *(*SDL_GL_GetProcAddress_fn)(const char *);
typedef __GLXextFuncPtr (*glXGetProcAddress_fn)(const GLubyte *);
typedef __eglMustCastToProperFunctionPointerType (*eglGetProcAddress_fn)(const char *);

static glXSwapBuffers_fn real_glXSwapBuffers;
static SDL_GL_SwapWindow_fn real_SDL_GL_SwapWindow;
static eglSwapBuffers_fn real_eglSwapBuffers;
static SDL_GL_GetProcAddress_fn real_SDL_GL_GetProcAddress;
static glXGetProcAddress_fn real_glXGetProcAddressARB;
static glXGetProcAddress_fn real_glXGetProcAddress;
static eglGetProcAddress_fn real_eglGetProcAddress;
static void *(*real_dlsym)(void *, const char *);

static pthread_once_t config_once = PTHREAD_ONCE_INIT;
static pthread_once_t dlsym_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t metrics_lock = PTHREAD_MUTEX_INITIALIZER;
static __thread unsigned int swap_depth;

static uint64_t *frame_times_ns;
static size_t frame_count;
static size_t frame_capacity;
static uint64_t warmup_ns;
static uint64_t interval_ns;
static uint64_t first_swap_ns;
static uint64_t previous_swap_ns;
static uint64_t interval_start_ns;
static FILE *metrics_output;

static void print_metrics(const char *format, ...) {
    va_list args;
    va_list output_args;

    va_start(args, format);
    if (metrics_output)
        va_copy(output_args, args);

    vfprintf(stderr, format, args);
    fflush(stderr);
    if (metrics_output) {
        vfprintf(metrics_output, format, output_args);
        fflush(metrics_output);
        va_end(output_args);
    }
    va_end(args);
}

static void resolve_real_dlsym(void) {
    /* egl_redirect.so interposes dlsym so it can return its GLX wrappers. If
     * this collector calls that wrapper with RTLD_NEXT, lookup begins after
     * egl_redirect.so instead of after fps_metrics.so and can resolve back to
     * this collector. Use glibc's versioned entry point so RTLD_NEXT retains
     * the caller ordering established by LD_PRELOAD. */
    real_dlsym = dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.2.5");
}

static void *lookup_symbol(void *handle, const char *name) {
    pthread_once(&dlsym_once, resolve_real_dlsym);
    return real_dlsym ? real_dlsym(handle, name) : NULL;
}

static void *resolve_symbol(const char *name, const char *library) {
    void *symbol = lookup_symbol(RTLD_NEXT, name);
    if (symbol)
        return symbol;

    // Source loads launcher.so and its SDL/libtogl dependencies with a local
    // dlopen scope, so RTLD_NEXT cannot see them from an LD_PRELOAD library.
    void *handle = dlopen(library, RTLD_LAZY | RTLD_NOLOAD);
    if (!handle)
        handle = dlopen(library, RTLD_LAZY | RTLD_LOCAL);
    return handle ? lookup_symbol(handle, name) : NULL;
}

static uint64_t monotonic_ns(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * NS_PER_SECOND + (uint64_t)now.tv_nsec;
}

static double read_seconds(const char *name, double fallback) {
    const char *value = getenv(name);
    if (!value || !*value)
        return fallback;

    char *end = NULL;
    double parsed = strtod(value, &end);
    if (!end || *end != '\0' || parsed < 0.0)
        return fallback;
    return parsed;
}

static void configure_metrics(void) {
    double interval_seconds = read_seconds("FPS_STATS_INTERVAL_SEC", DEFAULT_INTERVAL_SECONDS);
    double warmup_seconds = read_seconds("FPS_STATS_WARMUP_SEC", DEFAULT_WARMUP_SECONDS);
    const char *output_path = getenv("FPS_STATS_OUTPUT");

    if (interval_seconds <= 0.0)
        interval_seconds = DEFAULT_INTERVAL_SECONDS;

    interval_ns = (uint64_t)(interval_seconds * NS_PER_SECOND);
    warmup_ns = (uint64_t)(warmup_seconds * NS_PER_SECOND);

    if (output_path && *output_path) {
        metrics_output = fopen(output_path, "a");
        if (!metrics_output)
            fprintf(stderr, "[fps] warning: could not open output file %s\n", output_path);
    }

    print_metrics("[fps] measuring %.0f-second windows after %.0f-second warmup\n",
                  interval_seconds, warmup_seconds);
}

static int compare_u64(const void *left, const void *right) {
    uint64_t a = *(const uint64_t *)left;
    uint64_t b = *(const uint64_t *)right;
    return (a > b) - (a < b);
}

static int append_frame_time(uint64_t frame_ns) {
    if (frame_count == frame_capacity) {
        size_t new_capacity = frame_capacity ? frame_capacity * 2 : 8192;
        uint64_t *resized = realloc(frame_times_ns, new_capacity * sizeof(*resized));
        if (!resized)
            return 0;
        frame_times_ns = resized;
        frame_capacity = new_capacity;
    }

    frame_times_ns[frame_count++] = frame_ns;
    return 1;
}

static void print_interval_stats(uint64_t now_ns) {
    if (!frame_count)
        return;

    uint64_t total_ns = 0;
    for (size_t i = 0; i < frame_count; i++)
        total_ns += frame_times_ns[i];

    qsort(frame_times_ns, frame_count, sizeof(*frame_times_ns), compare_u64);

    size_t slow_count = (frame_count + 99) / 100;
    uint64_t slow_total_ns = 0;
    for (size_t i = frame_count - slow_count; i < frame_count; i++)
        slow_total_ns += frame_times_ns[i];

    double average_fps = total_ns ? (double)frame_count * NS_PER_SECOND / (double)total_ns : 0.0;
    double one_percent_low_fps = slow_total_ns
        ? (double)slow_count * NS_PER_SECOND / (double)slow_total_ns
        : 0.0;
    double elapsed_seconds = (double)(now_ns - interval_start_ns) / NS_PER_SECOND;

    print_metrics("[fps] %.1fs: avg %.2f FPS, 1%% low %.2f FPS (%zu frames)\n",
                  elapsed_seconds, average_fps, one_percent_low_fps, frame_count);

    frame_count = 0;
    interval_start_ns = now_ns;
}

static void record_swap(void) {
    pthread_once(&config_once, configure_metrics);

    uint64_t now_ns = monotonic_ns();
    pthread_mutex_lock(&metrics_lock);

    if (!first_swap_ns)
        first_swap_ns = now_ns;

    if (now_ns - first_swap_ns < warmup_ns) {
        pthread_mutex_unlock(&metrics_lock);
        return;
    }

    if (!previous_swap_ns) {
        previous_swap_ns = now_ns;
        interval_start_ns = now_ns;
        pthread_mutex_unlock(&metrics_lock);
        return;
    }

    uint64_t frame_ns = now_ns - previous_swap_ns;
    previous_swap_ns = now_ns;
    if (!append_frame_time(frame_ns)) {
        print_metrics("[fps] warning: could not allocate frame-time buffer\n");
        frame_count = 0;
        interval_start_ns = now_ns;
    } else if (now_ns - interval_start_ns >= interval_ns) {
        print_interval_stats(now_ns);
    }

    pthread_mutex_unlock(&metrics_lock);
}

static int begin_swap(void) {
    int outermost = swap_depth++ == 0;
    if (outermost)
        record_swap();
    return outermost;
}

static void end_swap(int outermost) {
    (void)outermost;
    swap_depth--;
}

void glXSwapBuffers(Display *display, GLXDrawable drawable) {
    if (!real_glXSwapBuffers)
        real_glXSwapBuffers =
            (glXSwapBuffers_fn)resolve_symbol("glXSwapBuffers", "libtogl.so");
    if (!real_glXSwapBuffers)
        return;

    int outermost = begin_swap();
    real_glXSwapBuffers(display, drawable);
    end_swap(outermost);
}

void SDL_GL_SwapWindow(void *window) {
    if (!real_SDL_GL_SwapWindow)
        real_SDL_GL_SwapWindow = (SDL_GL_SwapWindow_fn)resolve_symbol(
            "SDL_GL_SwapWindow", "libSDL2-2.0.so.0");
    if (!real_SDL_GL_SwapWindow)
        return;

    int outermost = begin_swap();
    real_SDL_GL_SwapWindow(window);
    end_swap(outermost);
}

EGLBoolean eglSwapBuffers(EGLDisplay display, EGLSurface surface) {
    if (!real_eglSwapBuffers)
        real_eglSwapBuffers =
            (eglSwapBuffers_fn)resolve_symbol("eglSwapBuffers", "libEGL.so.1");
    if (!real_eglSwapBuffers)
        return EGL_FALSE;

    int outermost = begin_swap();
    EGLBoolean result = real_eglSwapBuffers(display, surface);
    end_swap(outermost);
    return result;
}

void *SDL_GL_GetProcAddress(const char *name) {
    if (!real_SDL_GL_GetProcAddress)
        real_SDL_GL_GetProcAddress = (SDL_GL_GetProcAddress_fn)resolve_symbol(
            "SDL_GL_GetProcAddress", "libSDL2-2.0.so.0");

    if (name && strcmp(name, "glXSwapBuffers") == 0)
        return (void *)glXSwapBuffers;
    if (name && strcmp(name, "eglSwapBuffers") == 0)
        return (void *)eglSwapBuffers;
    return real_SDL_GL_GetProcAddress ? real_SDL_GL_GetProcAddress(name) : NULL;
}

__GLXextFuncPtr glXGetProcAddressARB(const GLubyte *name) {
    if (!real_glXGetProcAddressARB)
        real_glXGetProcAddressARB = (glXGetProcAddress_fn)resolve_symbol(
            "glXGetProcAddressARB", "libtogl.so");
    if (name && strcmp((const char *)name, "glXSwapBuffers") == 0)
        return (__GLXextFuncPtr)glXSwapBuffers;
    return real_glXGetProcAddressARB ? real_glXGetProcAddressARB(name) : NULL;
}

__GLXextFuncPtr glXGetProcAddress(const GLubyte *name) {
    if (!real_glXGetProcAddress)
        real_glXGetProcAddress = (glXGetProcAddress_fn)resolve_symbol(
            "glXGetProcAddress", "libtogl.so");
    if (name && strcmp((const char *)name, "glXSwapBuffers") == 0)
        return (__GLXextFuncPtr)glXSwapBuffers;
    return real_glXGetProcAddress ? real_glXGetProcAddress(name) : NULL;
}

__eglMustCastToProperFunctionPointerType eglGetProcAddress(const char *name) {
    if (!real_eglGetProcAddress)
        real_eglGetProcAddress = (eglGetProcAddress_fn)resolve_symbol(
            "eglGetProcAddress", "libEGL.so.1");
    if (name && strcmp(name, "eglSwapBuffers") == 0)
        return (__eglMustCastToProperFunctionPointerType)eglSwapBuffers;
    return real_eglGetProcAddress ? real_eglGetProcAddress(name) : NULL;
}
