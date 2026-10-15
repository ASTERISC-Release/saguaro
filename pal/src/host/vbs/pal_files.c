/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 Gramine contributors */

/*
 * Files of the vbs PAL proper. Three kinds:
 *
 *   - packed files: packed into the signed image at build time (scripts/vbs/pack.py), read-only,
 *     read from the packed file bytes (`.gxfile`). A directory exists for "/" and every prefix of
 *     a packed (or trusted) path. Mapping a packed ELF object where its load image already lies
 *     only adjusts the protection of its writable pages; any other mapping is a private copy into
 *     ordinary (pool) memory, which can never be executable;
 *   - trusted host files (manifest vbs.trusted_files): read whole from the host when opened,
 *     checked against their SHA-256, served from enclave memory, read-only;
 *   - allowed host files and directories (vbs.allowed_files): passed through to the host.
 *
 * Every other path does not exist (pal_hostio.c).
 */

#include "api.h"
#include "crypto.h"
#include "elf/elf.h"
#include "pal.h"
#include "pal_error.h"
#include "pal_internal.h"
#include "pal_linux_error.h"
#include "pal_vbs.h"

#define VBS_FILE_MODE 0555
#define VBS_DIR_MODE  0555
#define VBS_TRUSTED_MAX (1024ull * 1024 * 1024) /* largest trusted file read into the enclave */

#define L_O_RDONLY 0
#define L_O_WRONLY 01
#define L_O_RDWR   02
#define L_O_CREAT  0100
#define L_O_EXCL   0200

/* `path` is "/" or a proper prefix directory of a packed, trusted or allowed path */
static bool is_virtual_dir(const char* path) {
    if (!strcmp(path, "/"))
        return true;
    size_t len = strlen(path);
    for (uint64_t i = 0; i < g_vbs->nobjs; i++) {
        const char* p = g_vbs->objs[i].path;
        if (p[0] && !strncmp(p, path, len) && p[len] == '/')
            return true;
    }
    for (size_t i = 0; i < g_vbs_trusted_files_cnt; i++) {
        const char* p = g_vbs_trusted_files[i].path;
        if (!strncmp(p, path, len) && p[len] == '/')
            return true;
    }
    for (size_t i = 0; i < g_vbs_allowed_files_cnt; i++) {
        const char* p = g_vbs_allowed_files[i];
        if (!strncmp(p, path, len) && p[len] == '/' && p[len + 1])
            return true;
    }
    return false;
}

static void fill_attr(PAL_STREAM_ATTR* attr, bool dir, size_t size, uint32_t mode) {
    memset(attr, 0, sizeof(*attr));
    attr->handle_type  = dir ? PAL_TYPE_DIR : PAL_TYPE_FILE;
    attr->nonblocking  = false;
    attr->share_flags  = mode;
    attr->pending_size = size;
}

static int host_stat(const char* path, struct pal_vbs_host_stat* st) {
    size_t n = 0;
    int64_t r = vbs_hostcall(PAL_VBS_HOST_STAT, 0, 0, 0, 0, path, strlen(path), st, sizeof(*st),
                             &n);
    if (r < 0)
        return unix_to_pal_error((int)r);
    return n == sizeof(*st) ? 0 : -PAL_ERROR_DENIED;
}

static int open_dir_handle(PAL_HANDLE* handle, const char* path, bool host) {
    PAL_HANDLE hdl = calloc(1, HANDLE_SIZE(dir));
    if (!hdl)
        return -PAL_ERROR_NOMEM;
    init_handle_hdr(hdl, PAL_TYPE_DIR);
    hdl->dir.path = strdup(path);
    if (!hdl->dir.path) {
        free(hdl);
        return -PAL_ERROR_NOMEM;
    }
    hdl->dir.host = host;
    *handle = hdl;
    return 0;
}

static PAL_HANDLE new_file_handle(const char* path) {
    PAL_HANDLE hdl = calloc(1, HANDLE_SIZE(file));
    if (!hdl)
        return NULL;
    init_handle_hdr(hdl, PAL_TYPE_FILE);
    hdl->file.host_id = -1;
    if (path && !(hdl->file.path = strdup(path))) {
        free(hdl);
        return NULL;
    }
    return hdl;
}

/* a trusted file: read it whole from the host and check it against the manifest's hash */
static int open_trusted(PAL_HANDLE* handle, const char* path, const struct vbs_trusted_file* tf) {
    int64_t id = vbs_hostcall(PAL_VBS_HOST_OPEN, L_O_RDONLY, 0, 0, 0, path, strlen(path), NULL, 0,
                              NULL);
    if (id < 0)
        return unix_to_pal_error((int)id);

    size_t cap = 64 * 1024, size = 0;
    uint8_t* data = malloc(cap);
    int ret = 0;
    while (data) {
        if (size == cap) {
            if (cap >= VBS_TRUSTED_MAX) {
                ret = -PAL_ERROR_TOOLONG;
                break;
            }
            uint8_t* bigger = malloc(cap * 2);
            if (!bigger) {
                ret = -PAL_ERROR_NOMEM;
                break;
            }
            memcpy(bigger, data, size);
            free(data);
            data = bigger;
            cap *= 2;
        }
        size_t got = 0;
        size_t want = MIN(cap - size, (size_t)PAL_VBS_HOST_DATA_MAX);
        int64_t r = vbs_hostcall(PAL_VBS_HOST_PREAD, id, size, want, 0, NULL, 0, data + size, want,
                                 &got);
        if (r < 0) {
            ret = unix_to_pal_error((int)r);
            break;
        }
        if (got == 0)
            break;
        size += got;
    }
    vbs_hostcall(PAL_VBS_HOST_CLOSE, id, 0, 0, 0, NULL, 0, NULL, 0, NULL);
    if (!data)
        return -PAL_ERROR_NOMEM;
    if (ret < 0) {
        free(data);
        return ret;
    }

    LIB_SHA256_CONTEXT ctx;
    uint8_t hash[32];
    if (lib_SHA256Init(&ctx) < 0 || lib_SHA256Update(&ctx, data, size) < 0 ||
            lib_SHA256Final(&ctx, hash) < 0) {
        free(data);
        return -PAL_ERROR_DENIED;
    }
    if (memcmp(hash, tf->sha256, sizeof(hash))) {
        log_error("vbs: trusted file %s does not match its hash in the manifest", path);
        free(data);
        return -PAL_ERROR_DENIED;
    }

    PAL_HANDLE hdl = new_file_handle(path);
    if (!hdl) {
        free(data);
        return -PAL_ERROR_NOMEM;
    }
    hdl->file.data = data;
    hdl->file.size = size;
    *handle = hdl;
    return 0;
}

static int file_open(PAL_HANDLE* handle, const char* type, const char* uri, enum pal_access access,
                     pal_share_flags_t share, enum pal_create_mode create,
                     pal_stream_options_t options) {
    __UNUSED(options);
    if (strcmp(type, URI_TYPE_FILE))
        return -PAL_ERROR_INVAL;

    char path[URI_MAX];
    int ret = vbs_uri_to_path(uri, path, sizeof(path));
    if (ret < 0)
        return ret;

    const struct pal_vbs_packed_obj* obj = vbs_find_path(path);
    if (obj) {
        if (access != PAL_ACCESS_RDONLY || create == PAL_CREATE_ALWAYS)
            return -PAL_ERROR_DENIED; /* the packed files are part of the signed image */
        PAL_HANDLE hdl = new_file_handle(NULL);
        if (!hdl)
            return -PAL_ERROR_NOMEM;
        hdl->file.obj = obj;
        *handle = hdl;
        return 0;
    }

    const struct vbs_trusted_file* tf = vbs_find_trusted(path);
    if (tf) {
        if (access != PAL_ACCESS_RDONLY || create == PAL_CREATE_ALWAYS)
            return -PAL_ERROR_DENIED;
        return open_trusted(handle, path, tf);
    }

    if (vbs_is_allowed(path)) {
        struct pal_vbs_host_stat st;
        if (host_stat(path, &st) == 0 && st.is_dir) {
            if (access != PAL_ACCESS_RDONLY)
                return -PAL_ERROR_STREAMISDIR;
            return open_dir_handle(handle, path, /*host=*/true);
        }
        int64_t flags = access == PAL_ACCESS_WRONLY ? L_O_WRONLY
                        : access == PAL_ACCESS_RDWR ? L_O_RDWR : L_O_RDONLY;
        if (create == PAL_CREATE_TRY)
            flags |= L_O_CREAT;
        else if (create == PAL_CREATE_ALWAYS)
            flags |= L_O_CREAT | L_O_EXCL;
        int64_t id = vbs_hostcall(PAL_VBS_HOST_OPEN, flags, share, 0, 0, path, strlen(path), NULL,
                                  0, NULL);
        if (id < 0)
            return unix_to_pal_error((int)id);
        PAL_HANDLE hdl = new_file_handle(path);
        if (!hdl) {
            vbs_hostcall(PAL_VBS_HOST_CLOSE, id, 0, 0, 0, NULL, 0, NULL, 0, NULL);
            return -PAL_ERROR_NOMEM;
        }
        hdl->file.host_id = id;
        *handle = hdl;
        return 0;
    }

    if (is_virtual_dir(path) && access == PAL_ACCESS_RDONLY)
        return open_dir_handle(handle, path, /*host=*/false);
    return create == PAL_CREATE_NEVER ? -PAL_ERROR_STREAMNOTEXIST : -PAL_ERROR_DENIED;
}

static int64_t host_read(int64_t id, uint64_t offset, uint64_t count, void* buffer) {
    uint64_t done = 0;
    while (done < count) {
        size_t want = MIN(count - done, (uint64_t)PAL_VBS_HOST_DATA_MAX);
        size_t got = 0;
        int64_t r = vbs_hostcall(PAL_VBS_HOST_PREAD, id, offset + done, want, 0, NULL, 0,
                                 (uint8_t*)buffer + done, want, &got);
        if (r < 0)
            return done ? (int64_t)done : unix_to_pal_error((int)r);
        if (got == 0)
            break;
        done += got;
        if (got < want)
            break;
    }
    return (int64_t)done;
}

static int64_t file_read(PAL_HANDLE handle, uint64_t offset, uint64_t count, void* buffer) {
    const struct pal_vbs_packed_obj* obj = handle->file.obj;
    if (obj) {
        if (offset >= obj->file_size)
            return 0;
        uint64_t n = MIN(count, obj->file_size - offset);
        memcpy(buffer, vbs_file_data(obj) + offset, n);
        return (int64_t)n;
    }
    if (handle->file.data) {
        if (offset >= handle->file.size)
            return 0;
        uint64_t n = MIN(count, handle->file.size - offset);
        memcpy(buffer, handle->file.data + offset, n);
        return (int64_t)n;
    }
    if (handle->file.host_id < 0)
        return 0; /* an empty trusted file */
    return host_read(handle->file.host_id, offset, count, buffer);
}

static int64_t file_write(PAL_HANDLE handle, uint64_t offset, uint64_t count, const void* buffer) {
    if (handle->file.host_id < 0)
        return -PAL_ERROR_DENIED; /* packed or trusted: read-only */
    uint64_t done = 0;
    while (done < count) {
        size_t n = MIN(count - done, (uint64_t)PAL_VBS_HOST_DATA_MAX);
        int64_t r = vbs_hostcall(PAL_VBS_HOST_PWRITE, handle->file.host_id, offset + done, 0, 0,
                                 (const uint8_t*)buffer + done, n, NULL, 0, NULL);
        if (r < 0)
            return done ? (int64_t)done : unix_to_pal_error((int)r);
        if (r == 0 || (uint64_t)r > n)
            break;
        done += r;
    }
    return (int64_t)done;
}

static void file_destroy(PAL_HANDLE handle) {
    if (handle->file.host_id >= 0)
        vbs_hostcall(PAL_VBS_HOST_CLOSE, handle->file.host_id, 0, 0, 0, NULL, 0, NULL, 0, NULL);
    free(handle->file.data);
    free(handle->file.path);
    free(handle);
}

static int file_delete(PAL_HANDLE handle, enum pal_delete_mode delete_mode) {
    if (delete_mode != PAL_DELETE_ALL)
        return -PAL_ERROR_INVAL;
    if (handle->file.host_id < 0)
        return -PAL_ERROR_DENIED;
    int64_t r = vbs_hostcall(PAL_VBS_HOST_UNLINK, 0, 0, 0, 0, handle->file.path,
                             strlen(handle->file.path), NULL, 0, NULL);
    return r < 0 ? unix_to_pal_error((int)r) : 0;
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

    if (obj) {
        /* a dynamic loader's first mapping of a packed object: the whole extent at the packed
         * base, with the first segment's protection; the image is there already (its writable
         * segments become read-write when the loader maps them on their own) */
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
    }

    /* anywhere else, and every host file: a private copy of the file bytes in ordinary memory
     * (shared writable mappings of host files are not supported: changes stay private) */
    if (p & PAL_PROT_EXEC) {
        log_error("vbs: %s: executable file mappings exist only for packed objects at their "
                  "packed address (requested %p)", obj ? obj->path : handle->file.path, addr);
        return -PAL_ERROR_DENIED;
    }
    int ret = _PalVirtualMemoryAlloc(addr, size, PAL_PROT_READ | PAL_PROT_WRITE);
    if (ret < 0)
        return ret;
    int64_t got = file_read(handle, offset, size, addr);
    if (got < 0)
        return (int)got;
    if (p != (PAL_PROT_READ | PAL_PROT_WRITE))
        (void)vbs_protect(a, size, p);
    return 0;
}

static int file_setlength(PAL_HANDLE handle, uint64_t length) {
    if (handle->file.host_id < 0)
        return -PAL_ERROR_DENIED;
    int64_t r = vbs_hostcall(PAL_VBS_HOST_FTRUNCATE, handle->file.host_id, length, 0, 0, NULL, 0,
                             NULL, 0, NULL);
    return r < 0 ? unix_to_pal_error((int)r) : 0;
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
        fill_attr(attr, /*dir=*/false, obj->file_size, VBS_FILE_MODE);
        return 0;
    }
    if (vbs_find_trusted(path) || vbs_is_allowed(path)) {
        struct pal_vbs_host_stat st;
        ret = host_stat(path, &st);
        if (ret < 0)
            return ret;
        fill_attr(attr, st.is_dir, st.is_dir ? 0 : st.size,
                  vbs_find_trusted(path) ? 0444 : (uint32_t)st.mode);
        return 0;
    }
    if (is_virtual_dir(path)) {
        fill_attr(attr, /*dir=*/true, 0, VBS_DIR_MODE);
        return 0;
    }
    return -PAL_ERROR_STREAMNOTEXIST;
}

static int file_attrquerybyhdl(PAL_HANDLE handle, PAL_STREAM_ATTR* attr) {
    if (handle->file.obj) {
        fill_attr(attr, /*dir=*/false, handle->file.obj->file_size, VBS_FILE_MODE);
        return 0;
    }
    if (handle->file.host_id < 0) {
        fill_attr(attr, /*dir=*/false, handle->file.size, 0444); /* trusted */
        return 0;
    }
    struct pal_vbs_host_stat st;
    size_t n = 0;
    int64_t r = vbs_hostcall(PAL_VBS_HOST_FSTAT, handle->file.host_id, 0, 0, 0, NULL, 0, &st,
                             sizeof(st), &n);
    if (r < 0)
        return unix_to_pal_error((int)r);
    if (n != sizeof(st))
        return -PAL_ERROR_DENIED;
    fill_attr(attr, st.is_dir, st.size, (uint32_t)st.mode);
    return 0;
}

static int host_rename(char** path, const char* uri) {
    char newpath[URI_MAX];
    int ret = vbs_uri_to_path(uri, newpath, sizeof(newpath));
    if (ret < 0)
        return ret;
    if (!vbs_is_allowed(*path) || !vbs_is_allowed(newpath))
        return -PAL_ERROR_DENIED;
    size_t l1 = strlen(*path), l2 = strlen(newpath);
    char* both = malloc(l1 + l2 + 2);
    if (!both)
        return -PAL_ERROR_NOMEM;
    memcpy(both, *path, l1 + 1);
    memcpy(both + l1 + 1, newpath, l2);
    int64_t r = vbs_hostcall(PAL_VBS_HOST_RENAME, 0, 0, 0, 0, both, l1 + 1 + l2, NULL, 0, NULL);
    free(both);
    if (r < 0)
        return unix_to_pal_error((int)r);
    char* dup = strdup(newpath);
    if (dup) {
        free(*path);
        *path = dup;
    }
    return 0;
}

static int file_rename(PAL_HANDLE handle, const char* type, const char* uri) {
    __UNUSED(type);
    if (handle->file.host_id < 0)
        return -PAL_ERROR_DENIED;
    return host_rename(&handle->file.path, uri);
}

static int dir_open(PAL_HANDLE* handle, const char* type, const char* uri, enum pal_access access,
                    pal_share_flags_t share, enum pal_create_mode create,
                    pal_stream_options_t options) {
    __UNUSED(options);
    if (strcmp(type, URI_TYPE_DIR))
        return -PAL_ERROR_INVAL;
    char path[URI_MAX];
    int ret = vbs_uri_to_path(uri, path, sizeof(path));
    if (ret < 0)
        return ret;
    if (vbs_is_allowed(path)) {
        if (create == PAL_CREATE_TRY || create == PAL_CREATE_ALWAYS) {
            int64_t r = vbs_hostcall(PAL_VBS_HOST_MKDIR, share, 0, 0, 0, path, strlen(path), NULL,
                                     0, NULL);
            if (r < 0 && !(r == -17 /* EEXIST */ && create == PAL_CREATE_TRY))
                return unix_to_pal_error((int)r);
        }
        struct pal_vbs_host_stat st;
        ret = host_stat(path, &st);
        if (ret < 0)
            return ret;
        if (!st.is_dir)
            return -PAL_ERROR_STREAMNOTEXIST;
        return open_dir_handle(handle, path, /*host=*/true);
    }
    if (!is_virtual_dir(path))
        return create == PAL_CREATE_NEVER ? -PAL_ERROR_STREAMNOTEXIST : -PAL_ERROR_DENIED;
    if (access != PAL_ACCESS_RDONLY || create == PAL_CREATE_ALWAYS)
        return -PAL_ERROR_DENIED;
    return open_dir_handle(handle, path, /*host=*/false);
}

/* append `name` (a directory if `dir`) in the PAL's listing format; false if it does not fit */
static bool put_name(char** out, uint64_t* left, const char* name, size_t len, bool dir) {
    size_t need = len + (dir ? 1 : 0) + 1;
    if (need > *left)
        return false;
    memcpy(*out, name, len);
    if (dir)
        (*out)[len++] = '/';
    (*out)[len++] = '\0';
    *out += len;
    *left -= len;
    return true;
}

/* Lists the direct children of the directory: names separated by '\0', directories end in '/'.
 * Packed and trusted entries first, then the host directory of an allowed path. */
static int64_t dir_read(PAL_HANDLE handle, uint64_t offset, uint64_t count, void* buf) {
    __UNUSED(offset);
    const char* dir = handle->dir.path;
    size_t dlen = !strcmp(dir, "/") ? 0 : strlen(dir);
    char* out = buf;
    uint64_t left = count;

    /* packed paths, then trusted paths (the cursor runs over both) */
    uint64_t total = g_vbs->nobjs + g_vbs_trusted_files_cnt;
    for (; handle->dir.cursor < total; handle->dir.cursor++) {
        uint64_t i = handle->dir.cursor;
        const char* p = i < g_vbs->nobjs ? g_vbs->objs[i].path
                                         : g_vbs_trusted_files[i - g_vbs->nobjs].path;
        if (!p[0] || strncmp(p, dir, dlen) || p[dlen] != '/')
            continue;
        const char* name = p + dlen + 1;
        const char* slash = strchr(name, '/');
        size_t len = slash ? (size_t)(slash - name) : strlen(name);
        /* list a subdirectory once: only for the first path below it */
        bool seen = false;
        for (uint64_t j = 0; slash && j < i; j++) {
            const char* q = j < g_vbs->nobjs ? g_vbs->objs[j].path
                                             : g_vbs_trusted_files[j - g_vbs->nobjs].path;
            if (q[0] && !strncmp(q, p, dlen + 1 + len + 1))
                seen = true;
        }
        if (seen)
            continue;
        if (!put_name(&out, &left, name, len, slash != NULL)) {
            if (left == count)
                return -PAL_ERROR_OVERFLOW;
            return (int64_t)(count - left);
        }
    }

    if (handle->dir.host && !handle->dir.host_done) {
        char* names = malloc(PAL_VBS_HOST_DATA_MAX);
        if (!names)
            return -PAL_ERROR_NOMEM;
        size_t n = 0;
        int64_t cnt = vbs_hostcall(PAL_VBS_HOST_LISTDIR, handle->dir.host_cursor, 0, 0, 0, dir,
                                   strlen(dir), names, PAL_VBS_HOST_DATA_MAX, &n);
        if (cnt < 0) {
            free(names);
            return left == count ? unix_to_pal_error((int)cnt) : (int64_t)(count - left);
        }
        if (cnt == 0)
            handle->dir.host_done = true;
        size_t pos = 0;
        for (int64_t k = 0; k < cnt && pos < n; k++) {
            size_t len = strnlen(names + pos, n - pos);
            if (pos + len >= n)
                break;
            bool isdir = len && names[pos + len - 1] == '/';
            if (!put_name(&out, &left, names + pos, isdir ? len - 1 : len, isdir))
                break;
            handle->dir.host_cursor++;
            pos += len + 1;
        }
        free(names);
        if (left == count && cnt > 0)
            return -PAL_ERROR_OVERFLOW;
    }
    return (int64_t)(count - left);
}

static void dir_destroy(PAL_HANDLE handle) {
    free(handle->dir.path);
    free(handle);
}

static int dir_delete(PAL_HANDLE handle, enum pal_delete_mode delete_mode) {
    if (delete_mode != PAL_DELETE_ALL)
        return -PAL_ERROR_INVAL;
    if (!handle->dir.host)
        return -PAL_ERROR_DENIED;
    int64_t r = vbs_hostcall(PAL_VBS_HOST_UNLINK, /*is_dir=*/1, 0, 0, 0, handle->dir.path,
                             strlen(handle->dir.path), NULL, 0, NULL);
    return r < 0 ? unix_to_pal_error((int)r) : 0;
}

static int dir_attrquerybyhdl(PAL_HANDLE handle, PAL_STREAM_ATTR* attr) {
    __UNUSED(handle);
    fill_attr(attr, /*dir=*/true, 0, VBS_DIR_MODE);
    return 0;
}

static int dir_rename(PAL_HANDLE handle, const char* type, const char* uri) {
    __UNUSED(type);
    if (!handle->dir.host)
        return -PAL_ERROR_DENIED;
    return host_rename(&handle->dir.path, uri);
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
    if (HANDLE_TYPE(handle) != PAL_TYPE_FILE || !handle->file.obj || !handle->file.obj->image_size)
        return -PAL_ERROR_STREAMNOTEXIST;
    const struct pal_vbs_packed_obj* obj = handle->file.obj;
    g_vbs_libos_obj = obj; /* pal_rtld.c loads exactly one object: the LibOS */
    *out_base = (elf_addr_t)(uintptr_t)vbs_image_base(obj);
    *out_size = obj->map_size;
    *out_syscall_slot = obj->slot_offset ? *out_base + obj->slot_offset : 0;
    return 0;
}
