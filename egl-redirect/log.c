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

uint64_t ioctl_count = 0;
uint64_t ioctl_total_ns = 0;
uint64_t ioctl_max_ns = 0;
uint64_t frame_count = 0;
bool ioctl_logging_enabled = false;
#define IOCTL_STATS_MAX 64
typedef struct ioctl_stat {
  unsigned long request;
  uint64_t count;
  uint64_t total_ns;
  uint64_t max_ns;
  uint64_t last_fd;
} ioctl_stat;

static ioctl_stat ioctl_stats[IOCTL_STATS_MAX];
static size_t ioctl_stats_used = 0;
static uint64_t execbuffer2_last_flags = 0;
static uint32_t syncobj_wait_last_flags = 0;

/* Syscall logging toggle */
int syscall_logging_enabled = 0;

void set_syscall_logging(int enable) { syscall_logging_enabled = enable ? 1 : 0; }

static void print_syncobj_wait_flags(FILE *out, uint32_t flags) {
    if (flags == 0) {
        fprintf(out, "NONE");
        return;
    }

    bool first = true;
    #define PRINT_SYNCOBJ_FLAG(flag)                                            \
    do {                                                                       \
        if (flags & (flag)) {                                                    \
            fprintf(out, "%s%s", first ? "" : "|", #flag);                    \
            first = false;                                                         \
        }                                                                        \
    } while (0)

    PRINT_SYNCOBJ_FLAG(DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL);
    PRINT_SYNCOBJ_FLAG(DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT);

    if (first)
        fprintf(out, "0x%x", flags);

    #undef PRINT_SYNCOBJ_FLAG
}

static void print_execbuffer2_flags(FILE *out, uint64_t flags) {
    if (flags == 0) {
        fprintf(out, "NONE");
        return;
    }

    bool first = true;
    #define PRINT_FLAG(flag)                                                     \
    do {                                                                       \
        if (flags & (flag)) {                                                    \
            fprintf(out, "%s%s", first ? "" : "|", #flag);                     \
            first = false;                                                         \
        }                                                                        \
    } while (0)

    PRINT_FLAG(I915_EXEC_RING_MASK);
    PRINT_FLAG(I915_EXEC_DEFAULT);
    PRINT_FLAG(I915_EXEC_RENDER);
    PRINT_FLAG(I915_EXEC_BSD);
    PRINT_FLAG(I915_EXEC_BLT);
    PRINT_FLAG(I915_EXEC_VEBOX);
    PRINT_FLAG(I915_EXEC_SECURE);
    PRINT_FLAG(I915_EXEC_NO_RELOC);
    PRINT_FLAG(I915_EXEC_HANDLE_LUT);
    PRINT_FLAG(I915_EXEC_BSD_MASK);
    PRINT_FLAG(I915_EXEC_RESOURCE_STREAMER);
    PRINT_FLAG(I915_EXEC_FENCE_ARRAY);
    PRINT_FLAG(I915_EXEC_FENCE_OUT);
    PRINT_FLAG(I915_EXEC_USE_EXTENSIONS);
    #ifdef I915_EXEC_NO_FENCE
    PRINT_FLAG(I915_EXEC_NO_FENCE);
    #endif
    PRINT_FLAG(I915_EXEC_BATCH_FIRST);
    PRINT_FLAG(I915_EXEC_FENCE_SUBMIT);
    #ifdef I915_EXEC_CAPTURE
    PRINT_FLAG(I915_EXEC_CAPTURE);
    #endif
    #ifdef I915_EXEC_DEBUG
    PRINT_FLAG(I915_EXEC_DEBUG);
    #endif

    if (first)
    fprintf(out, "0x%llx", (unsigned long long)flags);

    #undef PRINT_FLAG
}

static const char *i915_ioctl_name(unsigned long request) {
    /* i915-specific ioctls */
    switch (request) {
    case DRM_IOCTL_I915_GEM_EXECBUFFER:
        return "GEM_EXECBUFFER";
    case DRM_IOCTL_I915_GEM_EXECBUFFER2:
        return "GEM_EXECBUFFER2";
    case DRM_IOCTL_I915_GEM_EXECBUFFER2_WR:
        return "GEM_EXECBUFFER2_WR";
    case DRM_IOCTL_I915_GEM_CREATE:
        return "GEM_CREATE";
    case DRM_IOCTL_I915_GEM_CREATE_EXT:
        return "GEM_CREATE_EXT";
    case DRM_IOCTL_I915_GEM_SET_DOMAIN:
        return "GEM_SET_DOMAIN";
    case DRM_IOCTL_I915_GEM_GET_TILING:
        return "GEM_GET_TILING";
    case DRM_IOCTL_I915_GEM_SET_TILING:
        return "GEM_SET_TILING";
    case DRM_IOCTL_I915_GEM_BUSY:
        return "GEM_BUSY";
    case DRM_IOCTL_I915_GEM_MMAP:
        return "GEM_MMAP";
    case DRM_IOCTL_I915_GEM_MMAP_GTT:
        return "GEM_MMAP_GTT";
    case DRM_IOCTL_I915_GEM_MMAP_OFFSET:
        return "GEM_MMAP_OFFSET";
    case DRM_IOCTL_I915_GEM_PREAD:
        return "GEM_PREAD";
    case DRM_IOCTL_I915_GEM_PWRITE:
        return "GEM_PWRITE";
    case DRM_IOCTL_I915_GEM_THROTTLE:
        return "GEM_THROTTLE";
    case DRM_IOCTL_I915_GEM_CONTEXT_CREATE:
        return "GEM_CONTEXT_CREATE";
    case DRM_IOCTL_I915_GEM_CONTEXT_CREATE_EXT:
        return "GEM_CONTEXT_CREATE_EXT";
    case DRM_IOCTL_I915_GEM_CONTEXT_DESTROY:
        return "GEM_CONTEXT_DESTROY";
    case DRM_IOCTL_I915_GEM_CONTEXT_SETPARAM:
        return "GEM_CONTEXT_SETPARAM";
    case DRM_IOCTL_I915_GEM_CONTEXT_GETPARAM:
        return "GEM_CONTEXT_GETPARAM";
    case DRM_IOCTL_I915_GEM_USERPTR:
        return "GEM_USERPTR";
    case DRM_IOCTL_I915_GEM_WAIT:
        return "GEM_WAIT";
    case DRM_IOCTL_I915_GEM_SW_FINISH:
        return "GEM_SW_FINISH";
    case DRM_IOCTL_I915_GEM_GET_APERTURE:
        return "GEM_GET_APERTURE";
    case DRM_IOCTL_I915_GEM_SET_CACHING:
        return "GEM_SET_CACHING";
    case DRM_IOCTL_I915_GEM_GET_CACHING:
        return "GEM_GET_CACHING";
    case DRM_IOCTL_I915_REG_READ:
        return "REG_READ";
    case DRM_IOCTL_I915_GETPARAM:
        return "GETPARAM";
    case DRM_IOCTL_I915_SETPARAM:
        return "SETPARAM";
    case DRM_IOCTL_I915_GEM_MADVISE:
        return "GEM_MADVISE";
    #ifdef DRM_IOCTL_I915_GEM_PIN
    case DRM_IOCTL_I915_GEM_PIN:
        return "GEM_PIN";
    #endif
    #ifdef DRM_IOCTL_I915_GEM_UNPIN
    case DRM_IOCTL_I915_GEM_UNPIN:
        return "GEM_UNPIN";
    #endif
    #ifdef DRM_IOCTL_I915_GEM_ENTERVT
    case DRM_IOCTL_I915_GEM_ENTERVT:
        return "GEM_ENTERVT";
    #endif
    #ifdef DRM_IOCTL_I915_GEM_LEAVEVT
    case DRM_IOCTL_I915_GEM_LEAVEVT:
        return "GEM_LEAVEVT";
    #endif
    #ifdef DRM_IOCTL_I915_GEM_SET_EXEC_TIMEOUT
    case DRM_IOCTL_I915_GEM_SET_EXEC_TIMEOUT:
        return "GEM_SET_EXEC_TIMEOUT";
    #endif
    #ifdef DRM_IOCTL_I915_GEM_GET_EXEC_TIMEOUT
    case DRM_IOCTL_I915_GEM_GET_EXEC_TIMEOUT:
        return "GEM_GET_EXEC_TIMEOUT";
    #endif
    #ifdef DRM_IOCTL_I915_GEM_GETPARAM
    case DRM_IOCTL_I915_GEM_GETPARAM:
        return "GEM_GETPARAM";
    #endif
    #ifdef DRM_IOCTL_I915_GEM_SHMEM_CREATE
    case DRM_IOCTL_I915_GEM_SHMEM_CREATE:
        return "GEM_SHMEM_CREATE";
    #endif
    #ifdef DRM_IOCTL_I915_GEM_CONTEXT_RESET_STATS
    case DRM_IOCTL_I915_GEM_CONTEXT_RESET_STATS:
        return "GEM_CONTEXT_RESET_STATS";
    #endif
    #ifdef DRM_IOCTL_I915_ALLOC
    case DRM_IOCTL_I915_ALLOC:
        return "ALLOC";
    #endif
    #ifdef DRM_IOCTL_I915_FREE
    case DRM_IOCTL_I915_FREE:
        return "FREE";
    #endif
    #ifdef DRM_IOCTL_I915_INIT
    case DRM_IOCTL_I915_INIT:
        return "INIT";
    #endif
    #ifdef DRM_IOCTL_I915_FLUSH
    case DRM_IOCTL_I915_FLUSH:
        return "FLUSH";
    #endif
    #ifdef DRM_IOCTL_I915_BATCHBUFFER
    case DRM_IOCTL_I915_BATCHBUFFER:
        return "BATCHBUFFER";
    #endif
    #ifdef DRM_IOCTL_I915_IRQ_EMIT
    case DRM_IOCTL_I915_IRQ_EMIT:
        return "IRQ_EMIT";
    #endif
    #ifdef DRM_IOCTL_I915_IRQ_WAIT
    case DRM_IOCTL_I915_IRQ_WAIT:
        return "IRQ_WAIT";
    #endif
    #ifdef DRM_IOCTL_I915_SWAP
    case DRM_IOCTL_I915_SWAP:
        return "SWAP";
    #endif
    #ifdef DRM_IOCTL_I915_CLIP
    case DRM_IOCTL_I915_CLIP:
        return "CLIP";
    #endif
    #ifdef DRM_IOCTL_I915_GEM_CONTEXT_RESET_STATS
    case DRM_IOCTL_I915_GEM_CONTEXT_RESET_STATS:
        return "GEM_CONTEXT_RESET_STATS";
    #endif
    #ifdef DRM_IOCTL_I915_GEM_VM_CREATE
    case DRM_IOCTL_I915_GEM_VM_CREATE:
        return "GEM_VM_CREATE";
    #endif
    #ifdef DRM_IOCTL_I915_GEM_VM_DESTROY
    case DRM_IOCTL_I915_GEM_VM_DESTROY:
        return "GEM_VM_DESTROY";
    #endif
    #ifdef DRM_IOCTL_I915_GEM_VM_BIND
    case DRM_IOCTL_I915_GEM_VM_BIND:
        return "GEM_VM_BIND";
    #endif
    #ifdef DRM_IOCTL_I915_GEM_VM_UNBIND
    case DRM_IOCTL_I915_GEM_VM_UNBIND:
        return "GEM_VM_UNBIND";
    #endif
    default:
        return NULL;
    }
}

static const char *drm_ioctl_name(unsigned long request) {
    /* Generic DRM ioctls (syncobj, etc.) */
    switch (request) {
    #ifdef DRM_IOCTL_SYNCOBJ_CREATE
    case DRM_IOCTL_SYNCOBJ_CREATE:
        return "SYNCOBJ_CREATE";
    #endif
    #ifdef DRM_IOCTL_SYNCOBJ_DESTROY
    case DRM_IOCTL_SYNCOBJ_DESTROY:
        return "SYNCOBJ_DESTROY";
    #endif
    #ifdef DRM_IOCTL_SYNCOBJ_WAIT
    case DRM_IOCTL_SYNCOBJ_WAIT:
        return "SYNCOBJ_WAIT";
    #endif
    #ifdef DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD
    case DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD:
        return "SYNCOBJ_HANDLE_TO_FD";
    #endif
    #ifdef DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE
    case DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE:
        return "SYNCOBJ_FD_TO_HANDLE";
    #endif
    #ifdef DRM_IOCTL_SYNCOBJ_RESET
    case DRM_IOCTL_SYNCOBJ_RESET:
        return "SYNCOBJ_RESET";
    #endif
    #ifdef DRM_IOCTL_SYNCOBJ_SIGNAL
    case DRM_IOCTL_SYNCOBJ_SIGNAL:
        return "SYNCOBJ_SIGNAL";
    #endif
    #ifdef DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT
    case DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT:
        return "SYNCOBJ_TIMELINE_WAIT";
    #endif
    #ifdef DRM_IOCTL_SYNCOBJ_QUERY
    case DRM_IOCTL_SYNCOBJ_QUERY:
        return "SYNCOBJ_QUERY";
    #endif
    #ifdef DRM_IOCTL_SYNCOBJ_EVENTFD
    case DRM_IOCTL_SYNCOBJ_EVENTFD:
        return "SYNCOBJ_EVENTFD";
    #endif
    default:
        return NULL;
    }
}

void print_ioctl_stats(void) {
    if (ioctl_count == 0) {
        fprintf(stderr, "IOCTL stats: no ioctls recorded in this interval\n");
        return;
    }

    /* Convert totals to microseconds for reporting */
    double total_us = (double)ioctl_total_ns / 1000.0;
    double avg_us = total_us / (double)ioctl_count;
    double max_us = (double)ioctl_max_ns / 1000.0;

    fprintf(stderr, "==== IOCTL STATS (last %lu frames) ===\n", (unsigned long)5000);
    fprintf(stderr, "Total IOCTLs: %lu\n", (unsigned long)ioctl_count);
    fprintf(stderr, "Total time: %.2f us\n", total_us);
    fprintf(stderr, "Average time: %.2f us\n", avg_us);
    fprintf(stderr, "Max time: %.2f us\n", max_us);
    fprintf(stderr, "Per-request breakdown:\n");

    fprintf(stderr, "last EXECBUFFER2 flags: 0x%llx (",
        (unsigned long long)execbuffer2_last_flags);
    print_execbuffer2_flags(stderr, execbuffer2_last_flags);
    fprintf(stderr, ")\n");

    fprintf(stderr, "last SYNCOBJ_WAIT flags: 0x%x (", syncobj_wait_last_flags);
    print_syncobj_wait_flags(stderr, syncobj_wait_last_flags);
    fprintf(stderr, ")\n");

    for (size_t i = 0; i < ioctl_stats_used; ++i) {
        ioctl_stat *s = &ioctl_stats[i];
        const char *name = i915_ioctl_name(s->request);
        if (!name)
        name = drm_ioctl_name(s->request);
        double total_req_us = (double)s->total_ns / 1000.0;
        double avg_req_us = (s->count ? (double)s->total_ns / (double)s->count / 1000.0 : 0.0);
        double max_req_us = (double)s->max_ns / 1000.0;
        if (name)
        fprintf(stderr, "  %s: count=%llu, total=%.2f us, avg=%.2f us, max=%.2f us, last_fd=%llu\n",
            name,
            (unsigned long long)s->count,
            total_req_us,
            avg_req_us,
            max_req_us,
            (unsigned long long)s->last_fd);
        else
        fprintf(stderr, "  0x%lx: count=%llu, total=%.2f us, avg=%.2f us, max=%.2f us, last_fd=%llu\n",
            (unsigned long)s->request,
            (unsigned long long)s->count,
            total_req_us,
            avg_req_us,
            max_req_us,
            (unsigned long long)s->last_fd);
    }

        fprintf(stderr,  "====================================\n");
    }

    static ioctl_stat *get_ioctl_stat(unsigned long request) {
    for (size_t i = 0; i < ioctl_stats_used; ++i) {
        if (ioctl_stats[i].request == request)
        return &ioctl_stats[i];
    }

    if (ioctl_stats_used >= IOCTL_STATS_MAX)
        return NULL;

    ioctl_stat *slot = &ioctl_stats[ioctl_stats_used++];
    memset(slot, 0, sizeof(*slot));
    slot->request = request;

    ioctl_count = 0;
    ioctl_total_ns = 0;
    ioctl_max_ns = 0;
    ioctl_stats_used = 0;
    execbuffer2_last_flags = 0;
    syncobj_wait_last_flags = 0;
    memset(ioctl_stats, 0, sizeof(ioctl_stats));

    return slot;
}

/* --- Syscall wrappers (log when syscall_logging_enabled) --- */
int open(const char *pathname, int flags, ...) {
  mode_t mode = 0;

  if (flags & O_CREAT) {
    va_list ap;
    va_start(ap, flags);
    mode = va_arg(ap, mode_t);
    va_end(ap);
    int ret = syscall(SYS_openat, AT_FDCWD, pathname, flags, mode);
    if (syscall_logging_enabled)
      fprintf(stderr, "[syscall] open(\"%s\", 0x%x, 0%o) = %d\n", pathname,
              flags, mode, ret);
    return ret;
  } else {
    int ret = syscall(SYS_openat, AT_FDCWD, pathname, flags);
    if (syscall_logging_enabled)
      fprintf(stderr, "[syscall] open(\"%s\", 0x%x) = %d\n", pathname,
              flags, ret);
    return ret;
  }
}

/* Additional open variants to intercept and log. These mirror the
 * behavior of `open` above, forwarding to the `openat` syscall and
 * handling the optional `mode` vararg when `O_CREAT` is present.
 */

int open64(const char *pathname, int flags, ...) {
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, mode_t);
        va_end(ap);
        int ret = syscall(SYS_openat, AT_FDCWD, pathname, flags, mode);
        if (syscall_logging_enabled)
            fprintf(stderr, "[syscall] open64(\"%s\", 0x%x, 0%o) = %d\n", pathname,
                            flags, mode, ret);
        return ret;
    } else {
        int ret = syscall(SYS_openat, AT_FDCWD, pathname, flags);
        if (syscall_logging_enabled)
            fprintf(stderr, "[syscall] open64(\"%s\", 0x%x) = %d\n", pathname,
                            flags, ret);
        return ret;
    }
}

int openat(int dirfd, const char *pathname, int flags, ...) {
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, mode_t);
        va_end(ap);
        int ret = syscall(SYS_openat, dirfd, pathname, flags, mode);
        if (syscall_logging_enabled)
            fprintf(stderr, "[syscall] openat(%d, \"%s\", 0x%x, 0%o) = %d\n", dirfd,
                            pathname, flags, mode, ret);
        return ret;
    } else {
        int ret = syscall(SYS_openat, dirfd, pathname, flags);
        if (syscall_logging_enabled)
            fprintf(stderr, "[syscall] openat(%d, \"%s\", 0x%x) = %d\n", dirfd,
                            pathname, flags, ret);
        return ret;
    }
}

int openat64(int dirfd, const char *pathname, int flags, ...) {
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, mode_t);
        va_end(ap);
        int ret = syscall(SYS_openat, dirfd, pathname, flags, mode);
        if (syscall_logging_enabled)
            fprintf(stderr, "[syscall] openat64(%d, \"%s\", 0x%x, 0%o) = %d\n", dirfd,
                            pathname, flags, mode, ret);
        return ret;
    } else {
        int ret = syscall(SYS_openat, dirfd, pathname, flags);
        if (syscall_logging_enabled)
            fprintf(stderr, "[syscall] openat64(%d, \"%s\", 0x%x) = %d\n", dirfd,
                            pathname, flags, ret);
        return ret;
    }
}

int creat(const char *pathname, mode_t mode) {
    int ret = syscall(SYS_creat, pathname, mode);
    if (syscall_logging_enabled)
        fprintf(stderr, "[syscall] creat(\"%s\", 0%o) = %d\n", pathname, mode, ret);
    return ret;
}

int creat64(const char *pathname, mode_t mode) {
    int ret = syscall(SYS_creat, pathname, mode);
    if (syscall_logging_enabled)
        fprintf(stderr, "[syscall] creat64(\"%s\", 0%o) = %d\n", pathname, mode, ret);
    return ret;
}

int close(int fd) {
  int ret = syscall(SYS_close, fd);
  if (syscall_logging_enabled)
    fprintf(stderr, "[syscall] close(%d) = %d\n", fd, ret);
  return ret;
}

int dup(int oldfd) {
  int ret = syscall(SYS_dup, oldfd);
  if (syscall_logging_enabled)
    fprintf(stderr, "[syscall] dup(%d) = %d\n", oldfd, ret);
  return ret;
}

int fcntl(int fd, int cmd, ...) {
  va_list ap;
  va_start(ap, cmd);
  unsigned long arg = va_arg(ap, unsigned long);
  va_end(ap);
  long ret = syscall(SYS_fcntl, fd, cmd, arg);
  if (syscall_logging_enabled)
    fprintf(stderr, "[syscall] fcntl(%d, %d, 0x%lx) = %ld\n", fd, cmd,
            arg, ret);
  return (int)ret;
}

ssize_t getdents64(int fd, void *dirp, size_t count) {
  ssize_t ret = syscall(SYS_getdents64, fd, dirp, count);
  if (syscall_logging_enabled)
    fprintf(stderr, "[syscall] getdents64(%d, %p, %zu) = %zd\n", fd, dirp,
            count, ret);
  return ret;
}

ssize_t readlink(const char *path, char *buf, size_t bufsiz) {
  ssize_t ret = syscall(SYS_readlink, path, buf, bufsiz);
  if (syscall_logging_enabled)
    fprintf(stderr, "[syscall] readlink(\"%s\", %p, %zu) = %zd\n", path,
            (void *)buf, bufsiz, ret);
  return ret;
}

/* Helper: print mmap flags in human-readable form. */
static void print_mmap_flags(FILE *out, int flags) {
    if (flags == 0) {
        fprintf(out, "NONE");
        return;
    }

    bool first = true;
#define PRINT_MMAP_FLAG(f) \
    do { if (flags & (f)) { fprintf(out, "%s%s", first ? "" : "|", #f); first = false; } } while (0)

    PRINT_MMAP_FLAG(MAP_PRIVATE);
    PRINT_MMAP_FLAG(MAP_SHARED);
#ifdef MAP_SHARED_VALIDATE
    PRINT_MMAP_FLAG(MAP_SHARED_VALIDATE);
#endif
#ifdef MAP_ANONYMOUS
    PRINT_MMAP_FLAG(MAP_ANONYMOUS);
#endif
#ifdef MAP_ANON
    PRINT_MMAP_FLAG(MAP_ANON);
#endif
    PRINT_MMAP_FLAG(MAP_FIXED);
#ifdef MAP_FIXED_NOREPLACE
    PRINT_MMAP_FLAG(MAP_FIXED_NOREPLACE);
#endif
#ifdef MAP_GROWSDOWN
    PRINT_MMAP_FLAG(MAP_GROWSDOWN);
#endif
#ifdef MAP_DENYWRITE
    PRINT_MMAP_FLAG(MAP_DENYWRITE);
#endif
#ifdef MAP_EXECUTABLE
    PRINT_MMAP_FLAG(MAP_EXECUTABLE);
#endif
#ifdef MAP_LOCKED
    PRINT_MMAP_FLAG(MAP_LOCKED);
#endif
#ifdef MAP_NORESERVE
    PRINT_MMAP_FLAG(MAP_NORESERVE);
#endif
#ifdef MAP_POPULATE
    PRINT_MMAP_FLAG(MAP_POPULATE);
#endif
#ifdef MAP_NONBLOCK
    PRINT_MMAP_FLAG(MAP_NONBLOCK);
#endif
#ifdef MAP_STACK
    PRINT_MMAP_FLAG(MAP_STACK);
#endif
#ifdef MAP_HUGETLB
    PRINT_MMAP_FLAG(MAP_HUGETLB);
#endif
#ifdef MAP_SYNC
    PRINT_MMAP_FLAG(MAP_SYNC);
#endif

    if (first)
        fprintf(out, "0x%x", flags);

#undef PRINT_MMAP_FLAG
}

/* Robust wrappers for mmap variants. Forward to the real libc symbols
 * when available via dlsym(RTLD_NEXT, ...). If resolving fails (for
 * example during early loader initialization) fall back to issuing the
 * syscall directly. Logging is enabled when `syscall_logging_enabled` is set.
 */

typedef void *(*mmap_fn_t)(void *, size_t, int, int, int, off_t);

void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset) {
    static mmap_fn_t real = NULL;
    if (!real) {
        real = (mmap_fn_t)dlsym(RTLD_NEXT, "mmap");
    }

    void *ret;
    if (real) {
        ret = real(addr, length, prot, flags, fd, offset);
    } else {
        ret = (void *)syscall(SYS_mmap, addr, length, prot, flags, fd, offset);
    }

    if (syscall_logging_enabled) {
        fprintf(stderr, "[syscall] mmap(%p, %zu, 0x%x, ", addr, length, prot);
        print_mmap_flags(stderr, flags);
        fprintf(stderr, ", %d, %" PRIu64 ") = %p\n", fd, (uint64_t)offset, ret);
    }
    return ret;
}

/* mmap64 and __mmap64 often forward to the same implementation on
 * some platforms. Provide explicit wrappers to catch calls to those
 * symbol names as well.
 */

void *mmap64(void *addr, size_t length, int prot, int flags, int fd, off_t offset) {
    static mmap_fn_t real64 = NULL;
    if (!real64) {
        real64 = (mmap_fn_t)dlsym(RTLD_NEXT, "mmap64");
    }

    void *ret;
    if (real64) {
        ret = real64(addr, length, prot, flags, fd, offset);
    } else {
        /* fallback to syscall; note: on many systems mmap and mmap64 are equivalent */
        ret = (void *)syscall(SYS_mmap, addr, length, prot, flags, fd, offset);
    }

    if (syscall_logging_enabled) {
        fprintf(stderr, "[syscall] mmap64(%p, %zu, 0x%x, ", addr, length, prot);
        print_mmap_flags(stderr, flags);
        fprintf(stderr, ", %d, %" PRIu64 ") = %p\n", fd, (uint64_t)offset, ret);
    }
    return ret;
}

void *__mmap64(void *addr, size_t length, int prot, int flags, int fd, off_t offset) {
    static mmap_fn_t real___64 = NULL;
    if (!real___64) {
        real___64 = (mmap_fn_t)dlsym(RTLD_NEXT, "__mmap64");
    }

    void *ret;
    if (real___64) {
        ret = real___64(addr, length, prot, flags, fd, offset);
    } else {
        ret = (void *)syscall(SYS_mmap, addr, length, prot, flags, fd, offset);
    }

    if (syscall_logging_enabled) {
        fprintf(stderr, "[syscall] __mmap64(%p, %zu, 0x%x, ", addr, length, prot);
        print_mmap_flags(stderr, flags);
        fprintf(stderr, ", %d, %" PRIu64 ") = %p\n", fd, (uint64_t)offset, ret);
    }
    return ret;
}

/* mmap2 is used on some 32-bit platforms where the offset is given in
 * 4096-byte pages. We still provide a wrapper to log calls; we don't
 * attempt to translate the pgoffset here, just log the raw value.
 */

void *mmap2(void *addr, size_t length, int prot, int flags, int fd, off_t pgoffset) {
    typedef void *(*mmap2_fn_t)(void *, size_t, int, int, int, off_t);
    static mmap2_fn_t real2 = NULL;
    if (!real2) {
        real2 = (mmap2_fn_t)dlsym(RTLD_NEXT, "mmap2");
    }

    void *ret;
    if (real2) {
        ret = real2(addr, length, prot, flags, fd, pgoffset);
    } else {
        /* There's no direct SYS_mmap2 on some systems; fall back to syscall mmap */
        ret = (void *)syscall(SYS_mmap, addr, length, prot, flags, fd, pgoffset * 4096ULL);
    }

    if (syscall_logging_enabled) {
        fprintf(stderr, "[syscall] mmap2(%p, %zu, 0x%x, ", addr, length, prot);
        print_mmap_flags(stderr, flags);
        fprintf(stderr, ", %d, %" PRIu64 ") = %p\n", fd, (uint64_t)pgoffset, ret);
    }
    return ret;
}

int fstat(int fd, struct stat *statbuf) {
  long ret = syscall(SYS_fstat, fd, statbuf);
  if (syscall_logging_enabled)
    fprintf(stderr, "[syscall] fstat(%d, %p) = %ld\n", fd, (void *)statbuf,
            ret);
  return (int)ret;
}

int fstatat(int fd, const char* file, struct stat *statbuf, int flags) {
  int ret = (int)syscall(SYS_newfstatat, fd, file, statbuf, flags);
  if (syscall_logging_enabled)
    fprintf(stderr, "[syscall] fstatat(%d, %s, %d) = %d\n", fd,
            (file ? file : "(NULL)"), flags, ret);
  return ret;
}

/* Some binaries reference newfstat; provide a thin wrapper to intercept it */
int newfstat(int fd, struct stat *statbuf) {
  int ret = (int)syscall(SYS_fstat, fd, statbuf);
  if (syscall_logging_enabled)
    fprintf(stderr, "[syscall] newfstat(%d, %p) = %d\n", fd,
            (void *)statbuf, ret);
  return ret;
}

/* Dumping rendered frames as .png images for verification */
void dump_ppm(const char *filename, int width, int height, unsigned char *pixels) {
    FILE *f = fopen(filename, "wb");
    if (!f) return;

    // PPM Header: P6 = Binary RGB, Width, Height, 255 = Max Color Value
    fprintf(f, "P6\n%d %d\n255\n", width, height);

    // OpenGL gives RGBA, but PPM wants RGB. 
    // We also flip it vertically here to fix the OpenGL coordinate system.
    for (int y = height - 1; y >= 0; y--) {
        for (int x = 0; x < width; x++) {
            unsigned char *p = &pixels[(y * width + x) * 4];
            fwrite(p, 1, 3, f); // Write only R, G, and B (skip A)
        }
    }
    fclose(f);
}