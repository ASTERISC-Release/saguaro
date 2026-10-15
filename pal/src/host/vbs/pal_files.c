/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 Gramine contributors */

/*
 * Files of the vbs PAL proper: the read-only namespace of the files packed into the signed image
 * at build time (scripts/vbs/pack.py). Reads come from the packed file bytes (`.gxfile`). A
 * directory exists for "/" and for every prefix of a packed path.
 *
 * Mapping a packed ELF object at the place its load image already occupies in the packed-images
 * section (what Gramine's loaders do once they have chosen that place) only adjusts the protection
 * of its writable pages; any other mapping is a private copy into ordinary (pool) memory, which can
 * never be executable.
 */

#include "api.h"
#include "elf/elf.h"
#include "pal.h"
#include "pal_error.h"
#include "pal_internal.h"
#include "pal_vbs.h"

#define VBS_FILE_MODE 0555
#define VBS_DIR_MODE  0555

/* is `path` "/" or a proper prefix directory of a packed path? */
static bool is_packed_dir(const char* path) {
    if (!strcmp(path, "/"))
        return true;
    size_t len = strlen(path);
    for (uint64_t i = 0; i < g_vbs->nobjs; i++) {
        const char* p = g_vbs->objs[i].path;
        if (p[0] && !strncmp(p, path, len) && p[len] == '/')
            return true;
    }
    return false;
}

static void fill_attr(PAL_STREAM_ATTR* attr, bool dir, size_t size) {
    memset(attr, 0, sizeof(*attr));
    attr->handle_type  = dir ? PAL_TYPE_DIR : PAL_TYPE_FILE;
    attr->nonblocking  = false;
    attr->share_flags  = dir ? VBS_DIR_MODE : VBS_FILE_MODE;
    attr->pending_size = size;
}

static int open_dir_handle(PAL_HANDLE* handle, const char* path) {
    PAL_HANDLE hdl = calloc(1, HANDLE_SIZE(dir));
    if (!hdl)
        return -PAL_ERROR_NOMEM;
    init_handle_hdr(hdl, PAL_TYPE_DIR);
    hdl->dir.path = strdup(path);
    if (!hdl->dir.path) {
        free(hdl);
        return -PAL_ERROR_NOMEM;
    }
    hdl->dir.cursor = 0;
    *handle = hdl;
    return 0;
}

static int file_open(PAL_HANDLE* handle, const char* type, const char* uri, enum pal_access access,
                     pal_share_flags_t share, enum pal_create_mode create,
                     pal_stream_options_t options) {
    __UNUSED(share);
    __UNUSED(options);
    if (strcmp(type, URI_TYPE_FILE))
        return -PAL_ERROR_INVAL;

    char path[URI_MAX];
    int ret = vbs_uri_to_path(uri, path, sizeof(path));
    if (ret < 0)
        return ret;

    const struct pal_vbs_packed_obj* obj = vbs_find_path(path);
    if (!obj) {
        if (is_packed_dir(path) && access == PAL_ACCESS_RDONLY)
            return open_dir_handle(handle, path);
        return create == PAL_CREATE_NEVER ? -PAL_ERROR_STREAMNOTEXIST : -PAL_ERROR_DENIED;
    }
    if (access != PAL_ACCESS_RDONLY || create == PAL_CREATE_ALWAYS)
        return -PAL_ERROR_DENIED; /* the packed files are part of the signed image */

    PAL_HANDLE hdl = calloc(1, HANDLE_SIZE(file));
    if (!hdl)
        return -PAL_ERROR_NOMEM;
    init_handle_hdr(hdl, PAL_TYPE_FILE);
    hdl->file.obj = obj;
    *handle = hdl;
    return 0;
}

static int64_t file_read(PAL_HANDLE handle, uint64_t offset, uint64_t count, void* buffer) {
    const struct pal_vbs_packed_obj* obj = handle->file.obj;
    if (offset >= obj->file_size)
        return 0;
    uint64_t n = MIN(count, obj->file_size - offset);
    memcpy(buffer, vbs_file_data(obj) + offset, n);
    return (int64_t)n;
}

static int64_t file_write(PAL_HANDLE handle, uint64_t offset, uint64_t count, const void* buffer) {
    __UNUSED(handle);
    __UNUSED(offset);
    __UNUSED(count);
    __UNUSED(buffer);
    return -PAL_ERROR_DENIED;
}

static void file_destroy(PAL_HANDLE handle) {
    free(handle);
}

static int file_delete(PAL_HANDLE handle, enum pal_delete_mode delete_mode) {
    __UNUSED(handle);
    __UNUSED(delete_mode);
    return -PAL_ERROR_DENIED;
}

/*
 * If [addr, addr+size) at file `offset` is exactly where the packed load image already holds that
 * part of the file (a PT_LOAD at the image base), return that PT_LOAD; else NULL.
 */
static const elf_phdr_t* packed_segment_at(const struct pal_vbs_packed_obj* obj, uintptr_t addr,
                                           uint64_t offset, uint64_t size) {
    if (!obj->image_size)
        return NULL;
    const uint8_t* base = vbs_image_base(obj);
    const elf_ehdr_t* eh = (const elf_ehdr_t*)base;
    const elf_phdr_t* ph = (const elf_phdr_t*)(base + eh->e_phoff);
    for (size_t i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD)
            continue;
        uintptr_t start = (uintptr_t)base + ALLOC_ALIGN_DOWN(ph[i].p_vaddr);
        uintptr_t end = (uintptr_t)base + ALLOC_ALIGN_UP(ph[i].p_vaddr + ph[i].p_memsz);
        if (addr == start && offset == ALLOC_ALIGN_DOWN(ph[i].p_offset) && size <= end - start)
            return &ph[i];
    }
    return NULL;
}

static int file_map(PAL_HANDLE handle, void* addr, pal_prot_flags_t prot, uint64_t offset,
                    uint64_t size) {
    const struct pal_vbs_packed_obj* obj = handle->file.obj;
    uintptr_t a = (uintptr_t)addr;
    pal_prot_flags_t p = prot & (PAL_PROT_READ | PAL_PROT_WRITE | PAL_PROT_EXEC);

    /* a dynamic loader's first mapping of a packed object: the whole extent at the packed base,
     * with the first segment's protection; the image is there already (its writable segments
     * become read-write when the loader maps them on their own) */
    if (obj->image_size && a == (uintptr_t)vbs_image_base(obj) && offset == 0 &&
            size <= ALLOC_ALIGN_UP(obj->map_size) && !(p & PAL_PROT_WRITE))
        return 0;

    const elf_phdr_t* seg = packed_segment_at(obj, a, offset, size);
    if (seg) {
        if (seg->p_flags & PF_X) {
            /* code: used in place, execute-read as signed */
            if (p & PAL_PROT_WRITE) {
                log_error("vbs: refusing a writable mapping of packed code (%p)", addr);
                return -PAL_ERROR_DENIED;
            }
            return 0;
        }
        if (p & PAL_PROT_EXEC) {
            log_error("vbs: refusing an executable mapping of packed data (%p)", addr);
            return -PAL_ERROR_DENIED;
        }
        if (seg->p_flags & PF_W) {
            /* data: the image already holds the file bytes; make the pages read-write */
            if (vbs_protect(a, size, PAL_PROT_READ | PAL_PROT_WRITE) < 0)
                return -PAL_ERROR_DENIED;
        }
        /* read-only data (no PF_W) stays as it is (execute-read image pages, never written) */
        return 0;
    }

    /* anywhere else: a private copy of the file bytes in ordinary memory */
    if (p & PAL_PROT_EXEC) {
        log_error("vbs: %s: executable file mappings exist only for packed objects at their "
                  "packed address (requested %p)", obj->path, addr);
        return -PAL_ERROR_DENIED;
    }
    int ret = _PalVirtualMemoryAlloc(addr, size, PAL_PROT_READ | PAL_PROT_WRITE);
    if (ret < 0)
        return ret;
    if (offset < obj->file_size)
        memcpy(addr, vbs_file_data(obj) + offset, MIN(size, obj->file_size - offset));
    if (p != (PAL_PROT_READ | PAL_PROT_WRITE))
        (void)vbs_protect(a, size, p);
    return 0;
}

static int file_setlength(PAL_HANDLE handle, uint64_t length) {
    __UNUSED(handle);
    __UNUSED(length);
    return -PAL_ERROR_DENIED;
}

static int file_flush(PAL_HANDLE handle) {
    __UNUSED(handle);
    return 0;
}

static int file_attrquery(const char* type, const char* uri, PAL_STREAM_ATTR* attr) {
    if (strcmp(type, URI_TYPE_FILE) && strcmp(type, URI_TYPE_DIR))
        return -PAL_ERROR_INVAL;
    char path[URI_MAX];
    int ret = vbs_uri_to_path(uri, path, sizeof(path));
    if (ret < 0)
        return ret;
    const struct pal_vbs_packed_obj* obj = vbs_find_path(path);
    if (obj) {
        fill_attr(attr, /*dir=*/false, obj->file_size);
        return 0;
    }
    if (is_packed_dir(path)) {
        fill_attr(attr, /*dir=*/true, 0);
        return 0;
    }
    return -PAL_ERROR_STREAMNOTEXIST;
}

static int file_attrquerybyhdl(PAL_HANDLE handle, PAL_STREAM_ATTR* attr) {
    fill_attr(attr, /*dir=*/false, handle->file.obj->file_size);
    return 0;
}

static int file_rename(PAL_HANDLE handle, const char* type, const char* uri) {
    __UNUSED(handle);
    __UNUSED(type);
    __UNUSED(uri);
    return -PAL_ERROR_DENIED;
}

static int dir_open(PAL_HANDLE* handle, const char* type, const char* uri, enum pal_access access,
                    pal_share_flags_t share, enum pal_create_mode create,
                    pal_stream_options_t options) {
    __UNUSED(share);
    __UNUSED(options);
    if (strcmp(type, URI_TYPE_DIR))
        return -PAL_ERROR_INVAL;
    char path[URI_MAX];
    int ret = vbs_uri_to_path(uri, path, sizeof(path));
    if (ret < 0)
        return ret;
    if (!is_packed_dir(path))
        return create == PAL_CREATE_NEVER ? -PAL_ERROR_STREAMNOTEXIST : -PAL_ERROR_DENIED;
    if (access != PAL_ACCESS_RDONLY || create == PAL_CREATE_ALWAYS)
        return -PAL_ERROR_DENIED;
    return open_dir_handle(handle, path);
}

/* Lists the direct children of the directory: names separated by '\0', directories end in '/'. */
static int64_t dir_read(PAL_HANDLE handle, uint64_t offset, uint64_t count, void* buf) {
    __UNUSED(offset);
    const char* dir = handle->dir.path;
    size_t dlen = !strcmp(dir, "/") ? 0 : strlen(dir);
    char* out = buf;
    int64_t written = 0;
    for (; handle->dir.cursor < g_vbs->nobjs; handle->dir.cursor++) {
        const char* p = g_vbs->objs[handle->dir.cursor].path;
        if (!p[0] || strncmp(p, dir, dlen) || p[dlen] != '/')
            continue;
        const char* name = p + dlen + 1;
        const char* slash = strchr(name, '/');
        size_t len = slash ? (size_t)(slash - name) : strlen(name);
        /* list a subdirectory once: only for the first packed path below it */
        bool seen = false;
        for (uint64_t j = 0; slash && j < handle->dir.cursor; j++) {
            const char* q = g_vbs->objs[j].path;
            if (q[0] && !strncmp(q, p, dlen + 1 + len + 1))
                seen = true;
        }
        if (seen)
            continue;
        size_t need = len + (slash ? 1 : 0) + 1;
        if (need > count - (uint64_t)written) {
            if (!written)
                return -PAL_ERROR_OVERFLOW;
            break;
        }
        memcpy(out, name, len);
        if (slash)
            out[len++] = '/';
        out[len++] = '\0';
        out += len;
        written += len;
    }
    return written;
}

static void dir_destroy(PAL_HANDLE handle) {
    free(handle->dir.path);
    free(handle);
}

static int dir_delete(PAL_HANDLE handle, enum pal_delete_mode delete_mode) {
    __UNUSED(handle);
    __UNUSED(delete_mode);
    return -PAL_ERROR_DENIED;
}

static int dir_attrquerybyhdl(PAL_HANDLE handle, PAL_STREAM_ATTR* attr) {
    __UNUSED(handle);
    fill_attr(attr, /*dir=*/true, 0);
    return 0;
}

static int dir_rename(PAL_HANDLE handle, const char* type, const char* uri) {
    __UNUSED(handle);
    __UNUSED(type);
    __UNUSED(uri);
    return -PAL_ERROR_DENIED;
}

struct handle_ops g_file_ops = {
    .open           = &file_open,
    .read           = &file_read,
    .write          = &file_write,
    .destroy        = &file_destroy,
    .delete         = &file_delete,
    .map            = &file_map,
    .setlength      = &file_setlength,
    .flush          = &file_flush,
    .attrquery      = &file_attrquery,
    .attrquerybyhdl = &file_attrquerybyhdl,
    .rename         = &file_rename,
};

struct handle_ops g_dir_ops = {
    .open           = &dir_open,
    .read           = &dir_read,
    .destroy        = &dir_destroy,
    .delete         = &dir_delete,
    .attrquery      = &file_attrquery,
    .attrquerybyhdl = &dir_attrquerybyhdl,
    .rename         = &dir_rename,
};

/* for pal_rtld.c (PAL_HOST_VBS): the load image of the packed object behind a file handle */
int vbs_packed_base(PAL_HANDLE handle, elf_addr_t* out_base, uintptr_t* out_size,
                    uintptr_t* out_syscall_slot) {
    if (HANDLE_TYPE(handle) != PAL_TYPE_FILE || !handle->file.obj->image_size)
        return -PAL_ERROR_STREAMNOTEXIST;
    const struct pal_vbs_packed_obj* obj = handle->file.obj;
    g_vbs_libos_obj = obj; /* pal_rtld.c loads exactly one object: the LibOS */
    *out_base = (elf_addr_t)(uintptr_t)vbs_image_base(obj);
    *out_size = obj->map_size;
    *out_syscall_slot = obj->slot_offset ? *out_base + obj->slot_offset : 0;
    return 0;
}
