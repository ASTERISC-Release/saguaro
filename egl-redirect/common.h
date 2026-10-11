#ifndef __COMMON_H__
#define __COMMON_H__

#define NUM_COMM_PAGES 1

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
extern check bufs[NUM_BUFFERS];

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

#define COMM_ADDR 0xf00000ULL
#define COMM_MAGIC 0x1234567812345678ULL

#define SYS_COMMS_ADDR 0xf00000ULL
#define SYS_COMMS_SIZE 4096 // One page width.

static const size_t THREE_GIGABYTES = 3ULL * 1024 * 1024 * 1024;
static const size_t ONE_GIGABYTE = 1ULL * 1024 * 1024 * 1024;
static const size_t DATA_SIZE = THREE_GIGABYTES;
static const size_t HUGE_DATA_SIZE = ONE_GIGABYTE;
static void *DATA_REGION = (void *)0x100000000ULL;
static void *HUGEPAGE_DATA_REGION = (void *)0x200000000ULL;

static const uint64_t X11_SETUP = 12;
static const uint64_t X11_PRESENT = 13;
static const uint64_t X11_IDLE_FENCE_BUFFER = 33;
static const uint64_t UPDATE_WINDOW_SIZE = 23;
static const uint64_t UPDATE_INPUT_MASK = 39;

static const uint64_t SYSCALL_LOGGING_ENABLE = 15;
static const uint64_t SYSCALL_LOGGING_DISABLE = 16;

typedef struct {
  volatile uint64_t magic;
  volatile uint64_t req_bit;
  volatile uint64_t ret;
  volatile uint64_t p1;
  volatile uint64_t p2;
  volatile uint64_t p3;
  volatile uint64_t p4;
  volatile uint64_t p5;
} comm_page_t;

_Static_assert(offsetof(comm_page_t, ret) == 16, "SG response must stay in the hot cache line");
_Static_assert(offsetof(comm_page_t, p5) == 56, "common SG payload must fit one cache line");
_Static_assert(sizeof(comm_page_t) == 64, "SG communication ABI must fit one cache line");

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
