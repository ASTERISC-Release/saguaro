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
 * A device ioctl forwarded to the host listener (saguaro-vm graphics redirection). Pointers inside
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
 * back afterwards and the application's pointer values are restored (in every bounced copy)
 * before any copy is written back. The sizes come from a count field (times an element size, or
 * times a size field), a byte-length field, or are fixed. Requests whose pointers depend on other
 * fields (AMDGPU_CS and AMDGPU_GEM_OP; the i915 extension chains, I915_QUERY items,
 * I915_GEM_EXECBUFFER2 and the context parameters) are handled in code below the table. The
 * layouts are those of the kernel's uapi headers (drm/drm.h, drm/amdgpu_drm.h, drm/i915_drm.h);
 * the set of requests is what Mesa's radeonsi/radv and iris/anv issue (dev docs:
 * graphics/FINDINGS.md, the i915 pointer table).
 */
enum ptr_dir { PTR_IN = 1, PTR_OUT = 2, PTR_INOUT = 3 };
enum cnt_kind { CNT_U32, CNT_U64, CNT_SIZET_BYTES, CNT_U32_BYTES, CNT_FIXED /* `elem` bytes */ };
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
    /* struct drm_i915_getparam { s32 param; s32 pad; int* value; }: one int, written */
    { 0xc0106446, {{8, 0, CNT_FIXED, 4, 0, PTR_OUT}}, 1 },
};
#define DRM_IOCTL_AMDGPU_CS_CMD 0xc0186444
#define AMDGPU_CHUNK_ID_BO_HANDLES_ID 0x06
/* union drm_amdgpu_gem_op { u32 handle; u32 op; u64 value; }: for GET_GEM_CREATE_INFO `value`
 * points to a struct drm_amdgpu_gem_create_in (32 bytes) the kernel fills; for SET_PLACEMENT it
 * is the value itself */
#define DRM_IOCTL_AMDGPU_GEM_OP_CMD 0xc0106450
#define AMDGPU_GEM_OP_GET_GEM_CREATE_INFO_ID 0
/* The user-pointer objects cannot be forwarded: the kernel pins the pages behind an address of
 * the listener's address space, not a copy, and the guest's addresses mean nothing there. */
#define DRM_IOCTL_AMDGPU_GEM_USERPTR_CMD 0xc0186451
#define DRM_IOCTL_I915_GEM_USERPTR_CMD 0xc0186473
/* i915 (drm/i915_drm.h) */
#define DRM_IOCTL_I915_QUERY_CMD 0xc0106479               /* struct drm_i915_query, 16 bytes */
#define DRM_IOCTL_I915_GEM_CREATE_EXT_CMD 0xc018647c      /* struct drm_i915_gem_create_ext, 24 */
#define DRM_IOCTL_I915_GEM_CONTEXT_CREATE_EXT_CMD 0xc010646d /* ..._context_create_ext, 16 */
#define DRM_IOCTL_I915_GEM_CONTEXT_GETPARAM_CMD 0xc0186474   /* struct drm_i915_gem_context_param */
#define DRM_IOCTL_I915_GEM_CONTEXT_SETPARAM_CMD 0xc0186475   /* (24 bytes) */
#define DRM_IOCTL_I915_GEM_EXECBUFFER2_CMD 0x40406469     /* struct drm_i915_gem_execbuffer2, 64 */
#define DRM_IOCTL_I915_GEM_EXECBUFFER2_WR_CMD 0xc0406469
#define I915_USER_EXTENSION_SIZE 32 /* struct i915_user_extension { u64 next; u32 name; ... } */
#define I915_GEM_CREATE_EXT_MEMORY_REGIONS_ID 0   /* 48 bytes: num_regions @36, regions @40 */
#define I915_GEM_CREATE_EXT_PROTECTED_CONTENT_ID 1 /* 40 bytes */
#define I915_GEM_CREATE_EXT_SET_PAT_ID 2          /* 40 bytes */
#define I915_CONTEXT_CREATE_EXT_SETPARAM_ID 0     /* 56 bytes: a drm_i915_gem_context_param @32 */
#define I915_CONTEXT_PARAM_ENGINES_ID 0xa
#define I915_EXEC_FENCE_ARRAY_FLAG 0x80000        /* cliprects_ptr: drm_i915_gem_exec_fence[] */
#define I915_EXEC_USE_EXTENSIONS_FLAG 0x200000    /* cliprects_ptr: an extension chain */
#define I915_EXEC_FENCE_IN_FLAG 0x10000           /* rsvd2: a sync_file fd (not translated) */
#define DRM_I915_GEM_EXECBUFFER_EXT_TIMELINE_FENCES_ID 0 /* 56 bytes: count @32, ptrs @40, @48 */
#define I915_EXT_CHAIN_MAX 16

struct ptr_fixup {
    void* user;      /* the application's buffer */
    void* copy;      /* its copy in the bounce */
    uint64_t* slot;  /* the pointer field (in a bounced structure) that was rewritten */
    size_t size;
    bool out;
};
#define MAX_FIXUPS 128

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
    fixups[*nfix] = (struct ptr_fixup){ .user = user, .copy = copy, .slot = pslot, .size = size,
                                        .out = out };
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
        case CNT_FIXED: return 1;
    }
    return 0;
}

/* a struct drm_i915_gem_context_param { u32 ctx_id; u32 size; u64 param; u64 value; } at `p`
 * (in a bounced structure): `value` is a pointer to `size` bytes when `size` is not zero (SSEU,
 * the engine map), the value itself otherwise */
static int translate_i915_context_param(struct gbounce* b, char* p, bool out,
                                        struct ptr_fixup* fixups, size_t* nfix) {
    uint32_t size = *(uint32_t*)(p + 4);
    uint64_t param = *(uint64_t*)(p + 8);
    if (!size)
        return 0;
    size_t n = *nfix;
    int ret = bounce_array(b, (uint64_t*)(p + 16), size, out, fixups, nfix);
    if (ret < 0 || n == *nfix)
        return ret;
    if (param == I915_CONTEXT_PARAM_ENGINES_ID && size >= 8 && *(uint64_t*)fixups[n].copy) {
        log_warning("forwarded ioctl: an i915 engine map with extensions is not supported");
        return -EINVAL; /* struct i915_context_param_engines: its own chain (load balancing) */
    }
    return 0;
}

enum i915_chain { CHAIN_GEM_CREATE, CHAIN_CONTEXT_CREATE, CHAIN_EXECBUF };

/* the linked extension blocks of the i915 requests: each starts with a struct i915_user_extension
 * { u64 next_extension; u32 name; u32 flags; u32 rsvd[4]; } and its size depends on `name`; the
 * kernel reads them (and the arrays some of them point to), so each is bounced in turn */
static int translate_i915_ext_chain(struct gbounce* b, uint64_t* slot, enum i915_chain kind,
                                    struct ptr_fixup* fixups, size_t* nfix) {
    for (int depth = 0; *slot; depth++) {
        if (depth >= I915_EXT_CHAIN_MAX)
            return -EINVAL;
        /* the header first: its name gives the block's size */
        if (!is_user_memory_readable((void*)(uintptr_t)*slot, I915_USER_EXTENSION_SIZE))
            return -EFAULT;
        uint32_t name = *(uint32_t*)((char*)(uintptr_t)*slot + 8);
        size_t size;
        switch (kind) {
            case CHAIN_GEM_CREATE:
                size = name == I915_GEM_CREATE_EXT_MEMORY_REGIONS_ID ? 48
                     : name == I915_GEM_CREATE_EXT_PROTECTED_CONTENT_ID ? 40
                     : name == I915_GEM_CREATE_EXT_SET_PAT_ID ? 40 : 0;
                break;
            case CHAIN_CONTEXT_CREATE:
                size = name == I915_CONTEXT_CREATE_EXT_SETPARAM_ID ? 56 : 0;
                break;
            case CHAIN_EXECBUF:
                size = name == DRM_I915_GEM_EXECBUFFER_EXT_TIMELINE_FENCES_ID ? 56 : 0;
                break;
            default:
                size = 0;
        }
        if (!size) {
            log_warning("forwarded ioctl: unknown i915 extension %u (chain %d)", name, (int)kind);
            return -EINVAL;
        }
        size_t n = *nfix;
        int ret = bounce_array(b, slot, size, /*out=*/false, fixups, nfix);
        if (ret < 0)
            return ret;
        char* ext = fixups[n].copy;
        if (kind == CHAIN_GEM_CREATE && name == I915_GEM_CREATE_EXT_MEMORY_REGIONS_ID) {
            /* regions[num_regions] of struct drm_i915_gem_memory_class_instance (4 bytes) */
            ret = bounce_array(b, (uint64_t*)(ext + 40), (size_t)*(uint32_t*)(ext + 36) * 4,
                               /*out=*/false, fixups, nfix);
        } else if (kind == CHAIN_CONTEXT_CREATE) {
            ret = translate_i915_context_param(b, ext + 32, /*out=*/false, fixups, nfix);
        } else if (kind == CHAIN_EXECBUF) {
            /* handles_ptr: fence_count x struct drm_i915_gem_exec_fence (8 bytes); values_ptr:
             * fence_count x u64 */
            size_t count = *(uint32_t*)(ext + 32);
            ret = bounce_array(b, (uint64_t*)(ext + 40), count * 8, /*out=*/false, fixups, nfix);
            if (ret == 0)
                ret = bounce_array(b, (uint64_t*)(ext + 48), count * 8, /*out=*/false, fixups,
                                   nfix);
        }
        if (ret < 0)
            return ret;
        slot = (uint64_t*)ext; /* next_extension, in the bounced copy */
    }
    return 0;
}

/* struct drm_i915_query { u32 num_items; u32 flags; u64 items_ptr; }: items[num_items] of struct
 * drm_i915_query_item { u64 query_id; s32 length; u32 flags; u64 data_ptr; } (24 bytes); the kernel
 * writes each item's length and, where length was given, `length` bytes at data_ptr */
static int translate_i915_query(struct gbounce* b, char* arg, struct ptr_fixup* fixups,
                                size_t* nfix) {
    uint32_t num_items = *(uint32_t*)arg;
    if (num_items > 64)
        return -EINVAL;
    size_t n = *nfix;
    int ret = bounce_array(b, (uint64_t*)(arg + 8), (size_t)num_items * 24, /*out=*/true, fixups,
                           nfix);
    if (ret < 0 || n == *nfix)
        return ret;
    char* items = fixups[n].copy;
    for (uint32_t i = 0; i < num_items; i++) {
        int32_t length = *(int32_t*)(items + i * 24 + 8);
        if (length <= 0)
            continue;
        ret = bounce_array(b, (uint64_t*)(items + i * 24 + 16), (size_t)length, /*out=*/true,
                           fixups, nfix);
        if (ret < 0)
            return ret;
    }
    return 0;
}

/* struct drm_i915_gem_execbuffer2 { u64 buffers_ptr; u32 buffer_count; u32 batch_start_offset;
 * u32 batch_len; u32 DR1; u32 DR4; u32 num_cliprects; u64 cliprects_ptr; u64 flags; u64 rsvd1;
 * u64 rsvd2; }: buffers[buffer_count] of struct drm_i915_gem_exec_object2 (56 bytes; the kernel
 * writes the offsets back), each with relocs_ptr -> relocation_count x 32 bytes (written back
 * too); cliprects_ptr is a fence array (I915_EXEC_FENCE_ARRAY) or an extension chain
 * (I915_EXEC_USE_EXTENSIONS) */
static int translate_i915_execbuffer2(struct gbounce* b, char* arg, struct ptr_fixup* fixups,
                                      size_t* nfix) {
    uint32_t buffer_count = *(uint32_t*)(arg + 8);
    uint64_t flags = *(uint64_t*)(arg + 40);
    if (flags & I915_EXEC_FENCE_IN_FLAG) {
        log_warning("forwarded ioctl: I915_EXEC_FENCE_IN is not supported");
        return -EINVAL;
    }
    size_t n = *nfix;
    int ret = bounce_array(b, (uint64_t*)arg, (size_t)buffer_count * 56, /*out=*/true, fixups,
                           nfix);
    if (ret < 0)
        return ret;
    if (n < *nfix) {
        char* objs = fixups[n].copy;
        for (uint32_t i = 0; i < buffer_count; i++) {
            char* obj = objs + i * 56;
            ret = bounce_array(b, (uint64_t*)(obj + 8), (size_t)*(uint32_t*)(obj + 4) * 32,
                               /*out=*/true, fixups, nfix);
            if (ret < 0)
                return ret;
        }
    }
    if (flags & I915_EXEC_USE_EXTENSIONS_FLAG)
        return translate_i915_ext_chain(b, (uint64_t*)(arg + 32), CHAIN_EXECBUF, fixups, nfix);
    if (flags & I915_EXEC_FENCE_ARRAY_FLAG)
        return bounce_array(b, (uint64_t*)(arg + 32), (size_t)*(uint32_t*)(arg + 28) * 8,
                            /*out=*/false, fixups, nfix);
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
    switch (cmd) {
        case DRM_IOCTL_AMDGPU_CS_CMD:
            return translate_amdgpu_cs(b, arg, fixups, nfix);
        case DRM_IOCTL_AMDGPU_GEM_OP_CMD:
            if (*(uint32_t*)(arg + 4) != AMDGPU_GEM_OP_GET_GEM_CREATE_INFO_ID)
                return 0;
            return bounce_array(b, (uint64_t*)(arg + 8), 32, /*out=*/true, fixups, nfix);
        case DRM_IOCTL_AMDGPU_GEM_USERPTR_CMD:
        case DRM_IOCTL_I915_GEM_USERPTR_CMD:
            log_warning("forwarded ioctl 0x%x: user-pointer objects are not available from a "
                        "demand-paged process", cmd);
            return -EFAULT;
        case DRM_IOCTL_I915_QUERY_CMD:
            return translate_i915_query(b, arg, fixups, nfix);
        case DRM_IOCTL_I915_GEM_CREATE_EXT_CMD:
            return translate_i915_ext_chain(b, (uint64_t*)(arg + 16), CHAIN_GEM_CREATE, fixups,
                                            nfix);
        case DRM_IOCTL_I915_GEM_CONTEXT_CREATE_EXT_CMD:
            return translate_i915_ext_chain(b, (uint64_t*)(arg + 8), CHAIN_CONTEXT_CREATE, fixups,
                                            nfix);
        case DRM_IOCTL_I915_GEM_CONTEXT_SETPARAM_CMD:
            return translate_i915_context_param(b, arg, /*out=*/false, fixups, nfix);
        case DRM_IOCTL_I915_GEM_CONTEXT_GETPARAM_CMD:
            return translate_i915_context_param(b, arg, /*out=*/true, fixups, nfix);
        case DRM_IOCTL_I915_GEM_EXECBUFFER2_CMD:
        case DRM_IOCTL_I915_GEM_EXECBUFFER2_WR_CMD:
            return translate_i915_execbuffer2(b, arg, fixups, nfix);
    }
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
    /* The pointer slots the translation rewrote live in bounce copies (the argument's or a nested
     * structure's): put the application's values back in all of them first, so that whatever is
     * copied back below carries the application's pointers, not bounce addresses. Then the
     * outputs (they do not overlap), then the argument itself. */
    for (size_t i = 0; i < nfix; i++)
        *fixups[i].slot = (uint64_t)(uintptr_t)fixups[i].user;
    for (size_t i = 0; i < nfix; i++)
        if (fixups[i].out && ret >= 0)
            memcpy(fixups[i].user, fixups[i].copy, fixups[i].size);
    if (arg_copy && arg_copy != (void*)arg && (_IOC_DIR(cmd) & _IOC_READ) &&
            is_user_memory_writable((void*)arg, arg_size))
        memcpy((void*)arg, arg_copy, arg_size);
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
