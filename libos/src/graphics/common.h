/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

#ifndef __COMMON_H__
#define __COMMON_H__

/*
 * The shared Tirith graphics ABI (the comm page, the request ids, the fallback
 * arena constants) comes from graphics_comm_abi.h, defined once for all three
 * consumers. This header adds the preload library's own externs and helpers;
 * the per-buffer state below mirrors the QEMU listener's `check` struct
 * (include/qemu/sg.h) by hand.
 */
#include "graphics_comm_abi.h"

/* Enable only when both the guest redirector and host listener should wait for
 * X11 Present idle fences before reusing redirected buffers. */
/* #define IDLE_FENCE_WAIT */

#define _GNU_SOURCE
#include <GL/gl.h>
#include <GL/glx.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <assert.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "input_ring.h"
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

#define NUM_BUFFERS 3
#define XCB_DRI3_PIXMAP_SCANOUT (1 << 0)
#define __NR_ACQUIRE_LLOCK 500
#define __NR_RELINQUISH_LLOCK 501
#define __NR_GRAPHICS_PHYSMEM 502

extern struct gbm_device *gbm;
#define DEFAULT_WIDTH 300
#define DEFAULT_HEIGHT 300

extern int win_width;
extern int win_height;

extern xcb_connection_t *conn;
extern xcb_window_t win;
extern bool dump_png;

/* Per-buffer state */
typedef struct buffer {
  struct gbm_bo *bo;
  int bo_fd;
  xcb_pixmap_t pixmap;
  EGLImageKHR image;
  GLuint tex;
  GLuint fbo;
  int shm_fence_fd;
  struct xshmfence *shm_fence;
  xcb_sync_fence_t sync_fence;
  int idle_fence_fd;
  struct xshmfence *idle_fence;
  xcb_sync_fence_t idle_sync_fence;
  GLuint rbo_depth;
} check;
/* the buffer descriptors the listener reads and writes: in physically contiguous memory when
 * the LibOS provides it (graphics_physmem(); a process with its own page tables), with the
 * address the listener gets in bufs_pa (equal to bufs otherwise) */
extern check* bufs;
extern uint64_t bufs_pa;
/* `size` bytes the host listener can read by their physical address `*out_pa`; NULL when the
 * LibOS has no such memory (identity-mapped memory: use any memory and its own address) */
void* graphics_physmem(size_t size, uint64_t* out_pa);

extern int cur;
extern xcb_window_t win;
extern xcb_sync_fence_t prev_present_fence;
void update_window_size_from_drawable(Display *dpy, GLXDrawable drawable);
void update_window(void);
void update_vm_window(GLXDrawable drawable);
void set_guest_app_window(xcb_window_t window);
void set_guest_app_window_creation_in_progress(bool in_progress);
bool consume_guest_app_window_change(void);
void use_native_presentation_window(GLXDrawable drawable);

/* =============== gramine defs/structs ===================== */
extern bool in_gramine_vm;
/* Starts the graphics session under the VM at the first graphics call (main.c). */
bool graphics_ensure_session(void);

/* The library-side comm region: one page at the guest address of the comm window. */
#define SYS_COMMS_ADDR 0xf00000ULL
#define SYS_COMMS_SIZE 4096

/* Fallback arena geometry (the runtime values are computed in saguaro.c from
 * the launcher's env); the hugepage arena follows the data arena unless
 * SAGUARO_HUGEPAGE_ADDR relocates it. */
static const size_t DATA_SIZE = GRAPHICS_GEM_DATA_SIZE;
static const size_t HUGE_DATA_SIZE = GRAPHICS_GEM_HUGEPAGE_SIZE;
static void *DATA_REGION = (void *)GRAPHICS_GEM_DATA_START;
static void *HUGEPAGE_DATA_REGION =
    (void *)(GRAPHICS_GEM_DATA_START + GRAPHICS_GEM_DATA_SIZE);



static inline comm_page_t *comm_page(long offset) {
  return (comm_page_t *)(uintptr_t)(COMM_ADDR + offset);
}

static uint64_t comm_sync_notify(comm_page_t *c) {
  if (!c || c->magic != COMM_MAGIC) {
    return 0;
  }
  /* Publish the request payload and request bit before polling for host
   * completion. The host accesses the same memory through a separate mapping;
   * volatile alone does not define an inter-thread/process synchronization
   * relationship. */
  __atomic_thread_fence(__ATOMIC_RELEASE);
  /* The dedicated listener completes SG requests synchronously; keep the round trip to a minimal
   * load/test loop instead of entering KVM's PAUSE-filter path. */
  while (__atomic_load_n(&c->req_bit, __ATOMIC_ACQUIRE) != 0) {
  }
  return c->ret;
}

/* =============== glx.c ===================== */
void setup_egl(void);
GLuint get_redirect_fbo(int buffer_index);

/* =============== glx.c ===================== */
extern int cur;
extern GLuint fbo;
static void check_egl_error(const char *where) {
  EGLint e = eglGetError();
  if (e != EGL_SUCCESS)
    fprintf(stderr, "EGL error at %s: 0x%04x\n", where, e);
}

extern EGLDisplay eglDpy;

typedef void *(*dlsym_fn_t)(void *, const char *);
extern dlsym_fn_t real_dlsym;

typedef void (*glXSwapBuffers_t)(Display *, GLXDrawable);
extern glXSwapBuffers_t real_glXSwapBuffers;
extern xcb_sync_fence_t prev_present_fence;

/* =============== log.c ===================== */
/* Syscall/ioctl logging toggle */
extern bool ioctl_logging_enabled;
extern int syscall_logging_enabled;
extern uint64_t ioctl_count;

/* Functions for logging */
void enable_syscall_logging(void);
void disable_syscall_logging(void);
void setup_syscall_comms(void);
void set_syscall_logging(int enable);
void print_ioctl_stats(void);
void dump_ppm(const char *filename, int width, int height,
              unsigned char *pixels);

/* xcb.c */
void create_pixmap_from_kbuf(check *bufs, int i, uint32_t size_bytes,
                             uint32_t stride);
int create_xcb_fence(check *bufs, int buf_index);
void create_and_setup_xcb_window(void);
int query_idle_fence_buffer(check *bufs, int cur);

long acquire_libos_lock(void);
long relinquish_libos_lock(long offset);

// #define BENCHMARKING 1
#define STATS_INTERVAL 5000
#endif
