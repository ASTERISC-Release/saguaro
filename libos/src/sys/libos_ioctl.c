/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2014 Stony Brook University */

/*
 * Implementation of system call "ioctl".
 */

#include "libos_handle.h"
#include "libos_internal.h"
#include "libos_process.h"
#include "libos_rwlock.h"
#include "libos_signal.h"
#include "libos_table.h"
#include "linux_abi/errors.h"
#include "linux_abi/ioctl.h"
#include "libos_device_ioctl.h"
#include "libos_graphics.h"
#include "pal.h"
#include "stat.h"

static void signal_io(IDTYPE caller, void* arg) {
    __UNUSED(caller);
    __UNUSED(arg);
    /* TODO: fill these values e.g. by getting the handle in arg; this is completely unusable now */
    siginfo_t info = {
        .si_signo = SIGIO,
        .si_code = SI_SIGIO,
        .si_band = 0,
        .si_fd = 0,
    };
    if (kill_current_proc(&info) < 0) {
        log_warning("signal_io: failed to deliver a signal");
    }
}

#ifdef ENABLE_GRAPHICS
/*
 * A device ioctl forwarded to the host listener (gramine-vm graphics redirection). Pointers inside
 * the argument need nothing (guest RAM is mapped at the same addresses in the listener), but the
 * fields listed in libos_device_ioctl.h do:
 *  - FD_IN: a LibOS fd of a forwarded host object (TYPE_HOST, installed at host fd +
 *    GRAPHICS_HOST_FD_OFFSET) is replaced by its host fd for the call; any other value is passed
 *    unchanged (the graphics preload library hands raw host fds to Mesa itself, see egl.c);
 *  - FD_OUT: the host fd the kernel wrote is installed as a new TYPE_HOST handle, and its LibOS fd
 *    is written back;
 *  - TIME_ABS: an absolute CLOCK_MONOTONIC deadline on Gramine's clock is made relative to "now",
 *    and the listener adds its own CLOCK_MONOTONIC (the guest cannot read the host's clock); the
 *    comm page's p4 names the field (offset + 1) and p5 carries GRAPHICS_IOCTL_TIME_REL_MAGIC.
 * Input fields are restored after the call, so the application sees its own values.
 */
static int install_host_fd(int host_fd, bool cloexec) {
    int fd = host_fd + (int)GRAPHICS_HOST_FD_OFFSET;
    int ret = graphics_drop_stale_host_handle(fd);
    if (ret < 0)
        return ret;
    struct libos_handle* hdl = get_new_handle();
    if (!hdl)
        return -ENOMEM;
    hdl->type = TYPE_HOST;
    ret = set_new_fd_handle_by_fd(fd, hdl, cloexec ? FD_CLOEXEC : 0, NULL);
    put_handle(hdl);
    return ret;
}

static void close_host_fd(int host_fd) {
    long _offset = libos_syscall_acquire_libos_lock();
    comm_page_t* c = comm_page(_offset);
    c->p1 = (uint64_t)host_fd + GRAPHICS_HOST_FD_OFFSET;
    c->req_bit = CLOSE;
    (void)comm_sync_notify(c);
    libos_syscall_relinquish_libos_lock(_offset);
}

/*
 * Pointers inside forwarded ioctl arguments (design-gpu.md, stage 3). The host kernel follows
 * them, so they must be physical: each pointed-to array is copied into the slot's bounce, the
 * field in the bounced argument is rewritten to the copy's physical address, outputs are copied
 * back afterwards and the application's pointer values are restored before the argument itself
 * is copied back. The sizes come from a count field (times an element size, or times a size
 * field) or a byte-length field. AMDGPU_CS nests three levels and is handled in code.
 */
enum ptr_dir { PTR_IN = 1, PTR_OUT = 2, PTR_INOUT = 3 };
enum cnt_kind { CNT_U32, CNT_U64, CNT_SIZET_BYTES, CNT_U32_BYTES };
struct ptr_field {
    uint16_t ptr_off;   /* the u64 pointer field */
    uint16_t cnt_off;   /* the count or byte-length field */
    uint8_t  cnt_kind;
    uint16_t elem;      /* element size (CNT_U32/CNT_U64), or 0: the size is in `elem_off` */
    uint16_t elem_off;  /* a u32 field holding the element size (AMDGPU_BO_LIST) */
    uint8_t  dir;
};
struct ptr_ioctl {
    unsigned int cmd;
    struct ptr_field fields[3];
    size_t fields_cnt;
};
#define DRM_SYNCOBJ_ARRAY(cmd_, cnt_off_) \
    { cmd_, {{0, cnt_off_, CNT_U32, 4, 0, PTR_IN}}, 1 }
static const struct ptr_ioctl g_ptr_ioctls[] = {
    /* struct drm_version: three string buffers with size_t lengths (written by the kernel) */
    { 0xc0406400, {{24, 16, CNT_SIZET_BYTES, 1, 0, PTR_OUT}, {40, 32, CNT_SIZET_BYTES, 1, 0, PTR_OUT},
                   {56, 48, CNT_SIZET_BYTES, 1, 0, PTR_OUT}}, 3 },
    /* struct drm_unique */
    { 0xc0106401, {{8, 0, CNT_SIZET_BYTES, 1, 0, PTR_OUT}}, 1 },
    /* struct drm_syncobj_wait: handles[count_handles] */
    DRM_SYNCOBJ_ARRAY(0xc02864c3, 16),
    /* struct drm_syncobj_array: RESET, SIGNAL */
    DRM_SYNCOBJ_ARRAY(0xc01064c4, 8),
    DRM_SYNCOBJ_ARRAY(0xc01064c5, 8),
    /* struct drm_syncobj_timeline_wait: handles and points */
    { 0xc03064ca, {{0, 24, CNT_U32, 4, 0, PTR_IN}, {8, 24, CNT_U32, 8, 0, PTR_IN}}, 2 },
    /* struct drm_syncobj_timeline_array: TIMELINE_SIGNAL (points in), QUERY (points out) */
    { 0xc01864cd, {{0, 16, CNT_U32, 4, 0, PTR_IN}, {8, 16, CNT_U32, 8, 0, PTR_IN}}, 2 },
    { 0xc01864cb, {{0, 16, CNT_U32, 4, 0, PTR_IN}, {8, 16, CNT_U32, 8, 0, PTR_INOUT}}, 2 },
    /* struct drm_amdgpu_info: return_pointer, return_size bytes */
    { 0x40206445, {{0, 8, CNT_U32_BYTES, 1, 0, PTR_OUT}}, 1 },
    /* union drm_amdgpu_wait_fences: fences[fence_count] of struct drm_amdgpu_fence (24 bytes) */
    { 0xc0186452, {{0, 8, CNT_U32, 24, 0, PTR_IN}}, 1 },
    /* union drm_amdgpu_bo_list: bo_info_ptr[bo_number] of bo_info_size bytes each */
    { 0xc0186443, {{16, 8, CNT_U32, 0, 12, PTR_IN}}, 1 },
    /* struct drm_amdgpu_gem_va (64-byte form): input_fence_syncobj_handles[num_syncobj_handles] */
    { 0xc0406448, {{56, 52, CNT_U32, 4, 0, PTR_IN}}, 1 },
};
#define DRM_IOCTL_AMDGPU_CS_CMD 0xc0186444
#define AMDGPU_CHUNK_ID_BO_HANDLES_ID 0x06

struct ptr_fixup {
    void* user;   /* the application's buffer */
    void* copy;   /* its copy in the bounce */
    size_t size;
    bool out;
};
#define MAX_FIXUPS 64

static const struct ptr_ioctl* find_ptr_ioctl(unsigned int cmd) {
    for (size_t i = 0; i < ARRAY_SIZE(g_ptr_ioctls); i++)
        if (g_ptr_ioctls[i].cmd == cmd)
            return &g_ptr_ioctls[i];
    return NULL;
}

/* copies a user array into the bounce and rewrites the pointer slot `pslot` (in a bounced
 * structure) to its physical address; records the copy for the output phase */
static int bounce_array(struct gbounce* b, uint64_t* pslot, size_t size, bool out,
                        struct ptr_fixup* fixups, size_t* nfix) {
    void* user = (void*)(uintptr_t)*pslot;
    if (!user || !size)
        return 0;
    if (!is_user_memory_readable(user, size) || (out && !is_user_memory_writable(user, size)))
        return -EFAULT;
    uint64_t pa;
    void* copy = graphics_bounce_out(b, user, size, &pa);
    if (!copy)
        return -ENOMEM;
    if (*nfix >= MAX_FIXUPS)
        return -ENOMEM;
    fixups[*nfix] = (struct ptr_fixup){ .user = user, .copy = copy, .size = size, .out = out };
    (*nfix)++;
    *pslot = pa;
    return 0;
}

static size_t field_count(const char* arg, const struct ptr_field* f) {
    switch (f->cnt_kind) {
        case CNT_U32: return *(const uint32_t*)(arg + f->cnt_off);
        case CNT_U64: return (size_t)*(const uint64_t*)(arg + f->cnt_off);
        case CNT_SIZET_BYTES: return *(const size_t*)(arg + f->cnt_off);
        case CNT_U32_BYTES: return *(const uint32_t*)(arg + f->cnt_off);
    }
    return 0;
}

/* AMDGPU_CS: chunks -> u64[num_chunks] of chunk pointers -> struct drm_amdgpu_cs_chunk
 * {chunk_id, length_dw, chunk_data} -> chunk_data (length_dw * 4 bytes); a BO_HANDLES chunk's data
 * is a struct drm_amdgpu_bo_list_in whose bo_info_ptr points to bo_number * bo_info_size bytes. */
static int translate_amdgpu_cs(struct gbounce* b, char* arg, struct ptr_fixup* fixups, size_t* nfix) {
    uint32_t num_chunks = *(uint32_t*)(arg + 8);
    uint64_t* chunks_slot = (uint64_t*)(arg + 16);
    if (!num_chunks || !*chunks_slot)
        return 0;
    if (num_chunks > 64)
        return -EINVAL;
    size_t n0 = *nfix;
    int ret = bounce_array(b, chunks_slot, num_chunks * sizeof(uint64_t), /*out=*/false, fixups, nfix);
    if (ret < 0)
        return ret;
    uint64_t* chunk_ptrs = fixups[n0].copy;
    for (uint32_t i = 0; i < num_chunks; i++) {
        size_t n1 = *nfix;
        ret = bounce_array(b, &chunk_ptrs[i], 16, /*out=*/false, fixups, nfix);
        if (ret < 0)
            return ret;
        char* chunk = fixups[n1].copy;
        uint32_t chunk_id = *(uint32_t*)chunk;
        uint32_t length_dw = *(uint32_t*)(chunk + 4);
        uint64_t* data_slot = (uint64_t*)(chunk + 8);
        size_t n2 = *nfix;
        ret = bounce_array(b, data_slot, (size_t)length_dw * 4, /*out=*/false, fixups, nfix);
        if (ret < 0)
            return ret;
        if (chunk_id == AMDGPU_CHUNK_ID_BO_HANDLES_ID && length_dw * 4 >= 24) {
            char* bo_list = fixups[n2].copy;
            uint32_t bo_number = *(uint32_t*)(bo_list + 8);
            uint32_t bo_info_size = *(uint32_t*)(bo_list + 12);
            ret = bounce_array(b, (uint64_t*)(bo_list + 16), (size_t)bo_number * bo_info_size,
                               /*out=*/false, fixups, nfix);
            if (ret < 0)
                return ret;
        }
    }
    return 0;
}

static int translate_pointers(unsigned int cmd, struct gbounce* b, char* arg, size_t arg_size,
                              struct ptr_fixup* fixups, size_t* nfix) {
    if (!b)
        return 0; /* identity-mapped memory: the kernel reaches the application's buffers */
    if (cmd == DRM_IOCTL_AMDGPU_CS_CMD)
        return translate_amdgpu_cs(b, arg, fixups, nfix);
    const struct ptr_ioctl* pi = find_ptr_ioctl(cmd);
    if (!pi)
        return 0;
    for (size_t i = 0; i < pi->fields_cnt; i++) {
        const struct ptr_field* f = &pi->fields[i];
        if ((size_t)f->ptr_off + 8 > arg_size)
            return -EINVAL;
        size_t count = field_count(arg, f);
        size_t elem = f->elem ? f->elem : *(const uint32_t*)(arg + f->elem_off);
        if (count && elem && count > SIZE_MAX / elem)
            return -EINVAL;
        int ret = bounce_array(b, (uint64_t*)(arg + f->ptr_off), count * elem,
                               !!(f->dir & PTR_OUT), fixups, nfix);
        if (ret < 0)
            return ret;
    }
    return 0;
}

static long forwarded_ioctl(unsigned int fd, unsigned int cmd, unsigned long arg) {
    const struct translated_ioctl* ti = find_translated_ioctl(cmd);
    int64_t saved[2] = {0};
    uint64_t time_field = 0;
    if (ti) {
        if (!is_user_memory_writable((void*)arg, _IOC_SIZE(cmd)))
            return -EFAULT;
        for (size_t i = 0; i < ti->fields_cnt; i++) {
            void* field = (void*)(arg + ti->fields[i].offset);
            switch (ti->fields[i].kind) {
                case FD_IN: {
                    saved[i] = *(int32_t*)field;
                    int in_fd = (int32_t)saved[i];
                    struct libos_handle* h = in_fd > (int)GRAPHICS_HOST_FD_OFFSET
                                             ? get_fd_handle(in_fd, NULL, NULL) : NULL;
                    if (h) {
                        if (h->type == TYPE_HOST)
                            *(int32_t*)field = in_fd - (int)GRAPHICS_HOST_FD_OFFSET;
                        put_handle(h);
                    }
                    break;
                }
                case TIME_ABS: {
                    saved[i] = *(int64_t*)field;
                    int64_t deadline = saved[i];
                    uint64_t now_us = 0;
                    if (deadline > 0 && deadline != INT64_MAX && PalSystemTimeQuery(&now_us) == 0) {
                        *(int64_t*)field = deadline - (int64_t)(now_us * 1000);
                        time_field = ti->fields[i].offset + 1;
                        log_debug("forwarded ioctl 0x%x: deadline %ld ns, %ld ns from now", cmd,
                                  (long)deadline, (long)*(int64_t*)field);
                    }
                    break;
                }
                case FD_OUT:
                    break;
            }
        }
    }

    long _offset = libos_syscall_acquire_libos_lock();
    comm_page_t* c = comm_page(_offset);
    /* the argument structure goes through the bounce (its size is in the request); pointers
     * inside it are the next step of design-gpu.md (stage 3: a per-request pointer table) */
    size_t arg_size = _IOC_SIZE(cmd);
    struct gbounce* b = arg_size ? graphics_bounce(_offset) : NULL;
    uint64_t arg_pa = arg;
    void* arg_copy = NULL;
    if (arg_size) {
        if (!is_user_memory_readable((void*)arg, arg_size)) {
            libos_syscall_relinquish_libos_lock(_offset);
            return -EFAULT;
        }
        arg_copy = graphics_bounce_out(b, (void*)arg, arg_size, &arg_pa);
        if (!arg_copy) {
            libos_syscall_relinquish_libos_lock(_offset);
            return -ENOMEM;
        }
    }
    struct ptr_fixup fixups[MAX_FIXUPS];
    size_t nfix = 0;
    if (arg_copy && arg_copy != (void*)arg) {
        int tret = translate_pointers(cmd, b, arg_copy, arg_size, fixups, &nfix);
        if (tret < 0) {
            libos_syscall_relinquish_libos_lock(_offset);
            return tret;
        }
    }
    c->p1 = (uint64_t)fd;
    c->p2 = (uint64_t)cmd;
    c->p3 = arg_pa;
    c->p4 = time_field;
    c->p5 = time_field ? GRAPHICS_IOCTL_TIME_REL_MAGIC : 0;
    c->req_bit = IOCTL;
    long ret = (long)comm_sync_notify(c);
    /* outputs back to the application, innermost last is fine (they do not overlap); the pointer
     * slots the translation rewrote live in bounce copies, so restore them there before any
     * structure is copied back: the copies of nested structures (chunks) are not copied back,
     * only the argument is */
    for (size_t i = 0; i < nfix; i++)
        if (fixups[i].out && ret >= 0)
            memcpy(fixups[i].user, fixups[i].copy, fixups[i].size);
    if (arg_copy && arg_copy != (void*)arg && (_IOC_DIR(cmd) & _IOC_READ) &&
            is_user_memory_writable((void*)arg, arg_size)) {
        /* the application's pointer values, not the bounce addresses */
        if (cmd == DRM_IOCTL_AMDGPU_CS_CMD) {
            uint64_t* slot = (uint64_t*)((char*)arg_copy + 16);
            if (nfix) *slot = (uint64_t)(uintptr_t)fixups[0].user;
        } else {
            const struct ptr_ioctl* pi = find_ptr_ioctl(cmd);
            size_t k = 0;
            for (size_t i = 0; pi && i < pi->fields_cnt; i++) {
                uint64_t* slot = (uint64_t*)((char*)arg_copy + pi->fields[i].ptr_off);
                if (*slot && k < nfix)
                    *slot = (uint64_t)(uintptr_t)fixups[k++].user;
            }
        }
        memcpy((void*)arg, arg_copy, arg_size);
    }
    libos_syscall_relinquish_libos_lock(_offset);

    if (!ti)
        return ret;
    for (size_t i = 0; i < ti->fields_cnt; i++) {
        void* field = (void*)(arg + ti->fields[i].offset);
        if (ti->fields[i].kind == FD_IN) {
            *(int32_t*)field = (int32_t)saved[i];
            continue;
        }
        if (ti->fields[i].kind == TIME_ABS) {
            *(int64_t*)field = saved[i];
            continue;
        }
        if (ret < 0)
            continue;
        int host_fd = *(int32_t*)field;
        if (host_fd < 0)
            continue;
        bool cloexec = true;
        if (ti->cloexec_flags_offset >= 0)
            cloexec = *(uint32_t*)(arg + ti->cloexec_flags_offset) & DRM_CLOEXEC;
        int new_fd = install_host_fd(host_fd, cloexec);
        if (new_fd < 0) {
            log_warning("forwarded ioctl 0x%x: cannot install host fd %d: %d", cmd, host_fd,
                        new_fd);
            close_host_fd(host_fd);
            *(int32_t*)field = -1;
            ret = new_fd;
            continue;
        }
        *(int32_t*)field = new_fd;
    }
    return ret;
}
#endif

/* `cmd` arrives as the full register: glibc passes the request as an unsigned long (zero-extended),
 * while the RISC-V calling convention lets the compiler assume a 32-bit argument is sign-extended,
 * so a request with bit 31 set (`_IOR`, e.g. RNDGETENTCNT) would compare unequal to its own
 * constant. Narrowing it explicitly makes the value well-defined on every architecture. */
long libos_syscall_ioctl(unsigned int fd, unsigned long cmd_reg, unsigned long arg) {
    unsigned int cmd = (unsigned int)cmd_reg;
    struct libos_handle_map* handle_map = get_thread_handle_map(NULL);
    assert(handle_map);

    struct libos_handle* hdl = get_fd_handle(fd, NULL, handle_map);
    if (!hdl)
        return -EBADF;

    #ifdef ENABLE_GRAPHICS
        if(hdl->type == TYPE_HOST)
        {
            put_handle(hdl);
            return forwarded_ioctl(fd, cmd, arg);
        }
    #endif
    int ret;

    lock(&g_dcache_lock);
    bool is_host_dev = hdl->type == TYPE_CHROOT && hdl->dentry->inode &&
        hdl->dentry->inode->type == S_IFCHR;
    unlock(&g_dcache_lock);

    if (is_host_dev) {
        ret = raw_device_ioctl(hdl, cmd, arg);
        goto out;
    }

    switch (cmd) {
        case TIOCGPGRP:
            if (!hdl->uri || strcmp(hdl->uri, URI_PREFIX_CONSOLE)) {
                ret = -ENOTTY;
                break;
            }

            if (!is_user_memory_writable((void*)arg, sizeof(int))) {
                ret = -EFAULT;
                break;
            }
            rwlock_read_lock(&g_process_id_lock);
            *(int*)arg = g_process.pgid;
            rwlock_read_unlock(&g_process_id_lock);
            ret = 0;
            break;
        case FIONBIO:
            if (!is_user_memory_readable((void*)arg, sizeof(int))) {
                ret = -EFAULT;
                break;
            }
            int nonblocking_on = *(int*)arg;
            ret = set_handle_nonblocking(hdl, !!nonblocking_on);
            break;
        case FIONCLEX:
            rwlock_write_lock(&handle_map->lock);
            if (HANDLE_ALLOCATED(handle_map->map[fd])) {
                handle_map->map[fd]->flags &= ~FD_CLOEXEC;
                ret = 0;
            } else {
                ret = -EBADF;
            }
            rwlock_write_unlock(&handle_map->lock);
            break;
        case FIOCLEX:
            rwlock_write_lock(&handle_map->lock);
            if (HANDLE_ALLOCATED(handle_map->map[fd])) {
                handle_map->map[fd]->flags |= FD_CLOEXEC;
                ret = 0;
            } else {
                ret = -EBADF;
            }
            rwlock_write_unlock(&handle_map->lock);
            break;
        case FIOASYNC:
            ret = install_async_event(hdl->pal_handle, 0, &signal_io, NULL);
            break;
        case FIONREAD: {
            if (!is_user_memory_writable((void*)arg, sizeof(int))) {
                ret = -EFAULT;
                break;
            }

            struct libos_fs* fs = hdl->fs;
            if (!fs || !fs->fs_ops) {
                ret = -ENOTTY;
                break;
            }

            if (fs->fs_ops->ioctl) {
                ret = fs->fs_ops->ioctl(hdl, cmd, arg);
                break;
            }

            /* TODO: the code below should be deleted and each handle type should have custom ioctl
             * handling. */
            int size = 0;
            if (fs->fs_ops->hstat) {
                struct stat stat;
                ret = fs->fs_ops->hstat(hdl, &stat);
                if (ret < 0)
                    break;
                size = stat.st_size;
            } else if (hdl->pal_handle) {
                PAL_STREAM_ATTR attr;
                ret = PalStreamAttributesQueryByHandle(hdl->pal_handle, &attr);
                if (ret < 0) {
                    ret = pal_to_unix_errno(ret);
                    break;
                }
                size = attr.pending_size;
            }

            int offset = 0;
            if (fs->fs_ops->seek) {
                ret = fs->fs_ops->seek(hdl, 0, SEEK_CUR);
                if (ret < 0)
                    break;
                offset = ret;
            }

            *(int*)arg = size - offset;
            ret = 0;
            break;
        }
        default: {
            struct libos_fs* fs = hdl->fs;
            if (!fs || !fs->fs_ops || !fs->fs_ops->ioctl) {
                ret = -ENOTTY;
                break;
            }
            ret = fs->fs_ops->ioctl(hdl, cmd, arg);
            break;
        }
    }

out:
    put_handle(hdl);
    if (ret == -EINTR) {
        ret = -ERESTARTSYS;
    }
    return ret;
}
