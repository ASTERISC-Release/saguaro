/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2023 Intel Corporation */


/*
 * This file contains operands to handle streams with URIs that start with "file:" or "dir:".
 */

#include "api.h"
#include "pal.h"
#include "pal_common.h"
#include "pal_common_tf.h"
#include "pal_error.h"
#include "pal_flags_conv.h"
#include "pal_internal.h"
#include "path_utils.h"
#include "stat.h"

#include "external/realpath.h"

#include "kernel_files.h"
#include "kernel_memory.h"
#include "kernel_virtio.h"

#define DIRBUF_SIZE 1024
#define DT_DIR      4

#ifdef __x86_64__
static struct memory_address_space* current_address_space(void) {
    struct pal_tcb_vm* tcb = (struct pal_tcb_vm*)pal_get_tcb();
    return tcb ? tcb->kernel_thread.address_space : NULL;
}

/*
 * Writable MAP_SHARED file mappings of processes with their own address spaces (experimental
 * multiprocess support). Every process mapping the same host file (same FUSE node) maps the same
 * frames, so stores are visible to all of them, as with Linux's page cache. The record of a node
 * also counts the PAL handles open on it. Rules:
 *   - read() and write() of a node with cached pages go through those pages (writes also go to the
 *     host file), so the file and its mappings agree inside the guest;
 *   - the LibOS writes a mapping back on msync() and munmap() (g_pal_public_state.
 *     file_maps_write_back), and a writable handle writes all cached pages back when it closes;
 *   - when the node's last handle closes, the cache drops its frames (frames still mapped stay
 *     with their mappings, as page-cache pages do).
 */
struct shared_file_cache {
    struct shared_file_cache* next;
    uint64_t nodeid;
    uint32_t handles;
    size_t frames_count;
    uintptr_t* frames; /* 0 = page not loaded yet */
};

static struct shared_file_cache* g_shared_file_caches;
static spinlock_t g_shared_file_caches_lock = INIT_SPINLOCK_UNLOCKED;

static struct shared_file_cache* shared_file_cache_find(uint64_t nodeid, bool create) {
    assert(spinlock_is_locked(&g_shared_file_caches_lock));
    struct shared_file_cache* cache = g_shared_file_caches;
    while (cache && cache->nodeid != nodeid)
        cache = cache->next;
    if (!cache && create) {
        cache = calloc(1, sizeof(*cache));
        if (!cache)
            return NULL;
        /* the record is shared by all processes: a child that creates it while it starts (when it
         * opens the LibOS binary, say) must not free it at its exit */
#ifdef PAL_HOST_VM
        pal_startup_allocs_forget(cache);
#endif
        cache->nodeid = nodeid;
        cache->next = g_shared_file_caches;
        g_shared_file_caches = cache;
    }
    return cache;
}

static void shared_file_note_open(uint64_t nodeid) {
    if (!memory_frame_pool_active())
        return;
    spinlock_lock(&g_shared_file_caches_lock);
    struct shared_file_cache* cache = shared_file_cache_find(nodeid, /*create=*/true);
    if (cache)
        cache->handles++;
    spinlock_unlock(&g_shared_file_caches_lock);
}

/* Takes a reference on every cached frame of the node (or, with `detach`, takes the cache's own
 * references and removes the node's record); the caller drops them with memory_frame_unref(). */
static uintptr_t* shared_file_collect(struct shared_file_cache* cache, bool detach,
                                      size_t* out_count) {
    assert(spinlock_is_locked(&g_shared_file_caches_lock));
    uintptr_t* frames = NULL;
    *out_count = 0;
    if (detach) {
        struct shared_file_cache** link = &g_shared_file_caches;
        while (*link != cache)
            link = &(*link)->next;
        *link = cache->next;
        frames = cache->frames;
        *out_count = cache->frames_count;
        free(cache);
        return frames;
    }
    if (!cache->frames_count)
        return NULL;
    frames = malloc(cache->frames_count * sizeof(*frames));
    if (!frames)
        return NULL;
    for (size_t i = 0; i < cache->frames_count; i++) {
        frames[i] = cache->frames[i];
        if (frames[i])
            memory_frame_ref(frames[i]);
    }
    *out_count = cache->frames_count;
    return frames;
}

/* Writes the cached pages that lie inside the file back to the host file. */
static void shared_file_write_back(struct pal_handle* handle, uintptr_t* frames, size_t count) {
    struct fuse_attr attr;
    if (virtio_fs_fuse_getattr(handle->file.nodeid, handle->file.fh, FUSE_GETATTR_FH, UINT64_MAX,
                               &attr) < 0)
        return;
    for (size_t i = 0; i < count && i * PAGE_SIZE < attr.size; i++) {
        if (!frames[i])
            continue;
        uint64_t len = MIN(PAGE_SIZE, attr.size - i * PAGE_SIZE);
        uint64_t written;
        int ret = virtio_fs_fuse_write(handle->file.nodeid, handle->file.fh,
                                       memory_frame_pointer(frames[i]), len, i * PAGE_SIZE,
                                       &written);
        if (ret < 0)
            log_warning("write-back of a shared mapping of '%s' failed: %s",
                        handle->file.realpath, pal_strerror(ret));
    }
}

static void shared_file_note_close(struct pal_handle* handle) {
    if (!memory_frame_pool_active())
        return;
    spinlock_lock(&g_shared_file_caches_lock);
    struct shared_file_cache* cache = shared_file_cache_find(handle->file.nodeid,
                                                             /*create=*/false);
    if (!cache) {
        spinlock_unlock(&g_shared_file_caches_lock);
        return;
    }
    if (cache->handles)
        cache->handles--;
    bool last = cache->handles == 0;
    size_t count = 0;
    uintptr_t* frames = NULL;
    if (last || handle->file.writable)
        frames = shared_file_collect(cache, /*detach=*/last, &count);
    spinlock_unlock(&g_shared_file_caches_lock);

    if (handle->file.writable && frames)
        shared_file_write_back(handle, frames, count);
    size_t cached = 0;
    for (size_t i = 0; i < count; i++) {
        if (frames[i]) {
            memory_frame_unref(frames[i]);
            cached++;
        }
    }
    free(frames);
    if (last && cached)
        log_debug("shared file cache of '%s' dropped (%zu pages); %zu frame-pool frames in use",
                  handle->file.realpath, cached, memory_frame_allocated_count());
}

/* Copies between `buffer` (file range [offset, offset + count)) and the node's cached pages: into
 * the buffer after a read, into the pages after a write. */
static void shared_file_sync_buffer(struct pal_handle* handle, uint64_t offset, uint64_t count,
                                    void* buffer, bool to_cache) {
    if (!count || !memory_frame_pool_active())
        return;
    for (uint64_t pos = offset; pos < offset + count;) {
        uint64_t page = pos / PAGE_SIZE;
        size_t in_page = pos % PAGE_SIZE;
        size_t chunk = MIN(PAGE_SIZE - in_page, offset + count - pos);
        uintptr_t frame = 0;
        spinlock_lock(&g_shared_file_caches_lock);
        struct shared_file_cache* cache = shared_file_cache_find(handle->file.nodeid,
                                                                 /*create=*/false);
        if (!cache || !cache->frames_count) {
            spinlock_unlock(&g_shared_file_caches_lock);
            return;
        }
        if (page < cache->frames_count && cache->frames[page]) {
            frame = cache->frames[page];
            memory_frame_ref(frame);
        }
        spinlock_unlock(&g_shared_file_caches_lock);
        if (frame) {
            char* cached = (char*)memory_frame_pointer(frame) + in_page;
            char* buf = (char*)buffer + (pos - offset);
            if (to_cache)
                memcpy(cached, buf, chunk);
            else
                memcpy(buf, cached, chunk);
            memory_frame_unref(frame);
        }
        pos += chunk;
    }
}

/* Returns the frame of page `page` of the file with a reference for the caller, loading it on
 * first use. */
static int shared_file_page(struct pal_handle* handle, uint64_t page, uintptr_t* out_frame) {
    uintptr_t frame = 0;
    spinlock_lock(&g_shared_file_caches_lock);
    struct shared_file_cache* cache = shared_file_cache_find(handle->file.nodeid,
                                                             /*create=*/true);
    if (!cache) {
        spinlock_unlock(&g_shared_file_caches_lock);
        return -PAL_ERROR_NOMEM;
    }
    if (page >= cache->frames_count) {
        size_t new_count = MAX(page + 1, cache->frames_count * 2);
        uintptr_t* frames = calloc(new_count, sizeof(*frames));
        if (!frames) {
            spinlock_unlock(&g_shared_file_caches_lock);
            return -PAL_ERROR_NOMEM;
        }
#ifdef PAL_HOST_VM
        pal_startup_allocs_forget(frames); /* shared, as the record */
#endif
        if (cache->frames_count)
            memcpy(frames, cache->frames, cache->frames_count * sizeof(*frames));
        free(cache->frames);
        cache->frames = frames;
        cache->frames_count = new_count;
    }
    frame = cache->frames[page];
    if (frame)
        memory_frame_ref(frame);
    spinlock_unlock(&g_shared_file_caches_lock);
    if (frame) {
        *out_frame = frame;
        return 0;
    }

    /* load outside the lock (the FUSE request may sleep); another process may win the race */
    int ret = memory_frame_alloc(&frame);
    if (ret < 0)
        return ret;
    ret = emulate_file_map_via_read(handle->file.nodeid, handle->file.fh,
                                    memory_frame_pointer(frame), page * PAGE_SIZE, PAGE_SIZE);
    if (ret < 0) {
        memory_frame_free(frame);
        return ret;
    }
    spinlock_lock(&g_shared_file_caches_lock);
    cache = shared_file_cache_find(handle->file.nodeid, /*create=*/false);
    if (cache && page < cache->frames_count && cache->frames[page]) {
        memory_frame_free(frame);
        frame = cache->frames[page];
        memory_frame_ref(frame);
    } else if (cache && page < cache->frames_count) {
        cache->frames[page] = frame; /* the cache's reference */
        memory_frame_ref(frame);     /* the caller's */
    } else {
        /* the record went away meanwhile (its last handle closed): the caller's frame only */
    }
    spinlock_unlock(&g_shared_file_caches_lock);
    *out_frame = frame;
    return 0;
}

static int shared_file_map(struct pal_handle* handle, struct memory_address_space* space,
                           void* addr, pal_prot_flags_t prot, uint64_t offset, uint64_t size) {
    bool execute = !!(prot & PAL_PROT_EXEC);
    for (uint64_t done = 0; done < size; done += PAGE_SIZE) {
        uintptr_t frame;
        int ret = shared_file_page(handle, (offset + done) / PAGE_SIZE, &frame);
        if (ret == 0) {
            ret = memory_address_space_map_shared_frame(space, (uintptr_t)addr + done, frame,
                                                        /*write=*/true, execute);
            memory_frame_unref(frame);
        }
        if (ret < 0) {
            if (done)
                (void)memory_address_space_free(space, addr, done);
            return ret;
        }
    }
    return memory_address_space_flush_range((uintptr_t)addr, size);
}
#endif

bool g_use_trusted_files = false; /* only TDX PAL will set this */

/* out_modified_path is allocated by this func; must be freed by the caller */
static int extract_dir_and_base(const char* orig_path, char** out_modified_path, char** out_dir,
                                char** out_base) {
    char* modified_path = *orig_path == '/' ? strdup(orig_path)
                                            : alloc_concat("./", -1, orig_path, -1);
    if (!modified_path)
        return -PAL_ERROR_NOMEM;

    /* first remove trailing `/` symbols, then find the last `/` delimeter between dir and base */
    size_t len = strlen(modified_path);
    while (len > 0) {
        len--;
        if (modified_path[len] == '/')
            modified_path[len] = '\0';
        else
            break;
    }
    while (len > 0) {
        len--;
        if (modified_path[len] == '/') {
            modified_path[len] = '\0';
            *out_modified_path = modified_path;
            *out_dir  = &modified_path[0];
            *out_base = &modified_path[len + 1];
            return 0;
        }
    }

    free(modified_path);
    return -PAL_ERROR_INVAL;
}

static int file_or_dir_rename(const char* old, const char* new, uint32_t flags) {
    int ret;
    char* old_realpath_with_slash = NULL;
    char* old_resolved_path = NULL;
    char* new_realpath_with_slash = NULL;
    char* new_resolved_path = NULL;

    char* old_base_path = NULL;
    char* old_dir_path = NULL;
    ret = extract_dir_and_base(old, &old_realpath_with_slash, &old_dir_path, &old_base_path);
    if (ret < 0)
        goto out;

    ret = realpath(old_dir_path, /*got_path=*/NULL, &old_resolved_path);
    if (ret < 0)
        goto out;

    uint64_t old_dir_nodeid;
    ret = virtio_fs_fuse_lookup(old_resolved_path, &old_dir_nodeid, /*out_attr=*/NULL);
    if (ret < 0)
        goto out;

    char* new_base_path = NULL;
    char* new_dir_path = NULL;
    ret = extract_dir_and_base(new, &new_realpath_with_slash, &new_dir_path, &new_base_path);
    if (ret < 0)
        goto out;

    ret = realpath(new_dir_path, /*got_path=*/NULL, &new_resolved_path);
    if (ret < 0)
        goto out;

    uint64_t new_dir_nodeid;
    ret = virtio_fs_fuse_lookup(new_resolved_path, &new_dir_nodeid, /*out_attr=*/NULL);
    if (ret < 0)
        goto out;

    ret = flags ? virtio_fs_fuse_rename2(old_dir_nodeid, old_base_path, new_dir_nodeid,
                                         new_base_path, flags)
                : virtio_fs_fuse_rename(old_dir_nodeid, old_base_path, new_dir_nodeid,
                                        new_base_path);
    if (ret < 0)
        goto out;

    ret = 0;
out:
    free(old_realpath_with_slash);
    free(old_resolved_path);
    free(new_realpath_with_slash);
    free(new_resolved_path);
    return ret;
}

int pal_common_file_unlink_path(const char* uri) {
    int ret;
    char* modified = NULL;
    char* dir = NULL;
    char* base = NULL;
    char* resolved_dir = NULL;
    ret = extract_dir_and_base(uri, &modified, &dir, &base);
    if (ret < 0)
        goto out;
    ret = realpath(dir, /*got_path=*/NULL, &resolved_dir);
    if (ret < 0)
        goto out;
    uint64_t dir_nodeid;
    ret = virtio_fs_fuse_lookup(resolved_dir, &dir_nodeid, /*out_attr=*/NULL);
    if (ret < 0)
        goto out;
    ret = virtio_fs_fuse_unlink(dir_nodeid, base);
out:
    free(modified);
    free(resolved_dir);
    return ret;
}

int pal_common_file_rename_path(const char* old_uri, const char* new_uri) {
    return file_or_dir_rename(old_uri, new_uri, /*flags=*/0);
}

int pal_common_file_exchange_path(const char* uri_a, const char* uri_b) {
    return file_or_dir_rename(uri_a, uri_b, /*RENAME_EXCHANGE=*/2);
}

/* The node of `uri` itself (its last component is not followed): the directory part is resolved,
 * then the full path is looked up. */
static int lookup_nofollow(const char* uri, uint64_t* out_nodeid, struct fuse_attr* out_attr) {
    char* modified = NULL;
    char* dir = NULL;
    char* base = NULL;
    char* resolved_dir = NULL;
    char* full = NULL;
    int ret = extract_dir_and_base(uri, &modified, &dir, &base);
    if (ret < 0)
        goto out;
    ret = realpath(dir, /*got_path=*/NULL, &resolved_dir);
    if (ret < 0)
        goto out;
    full = alloc_concat3(resolved_dir, -1, "/", 1, base, -1);
    if (!full) {
        ret = -PAL_ERROR_NOMEM;
        goto out;
    }
    ret = virtio_fs_fuse_lookup(full, out_nodeid, out_attr);
out:
    free(modified);
    free(resolved_dir);
    free(full);
    return ret;
}

int pal_common_file_readlink_path(const char* uri, char* buf, size_t* inout_size) {
    uint64_t nodeid;
    struct fuse_attr attr = {0};
    int ret = lookup_nofollow(uri, &nodeid, &attr);
    if (ret < 0)
        return ret;
    if ((attr.mode & S_IFMT) != S_IFLNK)
        return -PAL_ERROR_INVAL;
    uint64_t size = 0;
    ret = virtio_fs_fuse_readlink(nodeid, *inout_size, buf, &size);
    if (ret < 0)
        return ret;
    *inout_size = size;
    return 0;
}

int pal_common_file_xattr(const char* uri, enum pal_xattr_op op, const char* name, void* value,
                          size_t* inout_size, int flags) {
    uint64_t nodeid;
    int ret;
    if (flags & PAL_XATTR_NOFOLLOW) {
        ret = lookup_nofollow(uri, &nodeid, /*out_attr=*/NULL);
    } else {
        char* resolved = NULL;
        ret = realpath(uri, /*got_path=*/NULL, &resolved);
        if (ret == 0)
            ret = virtio_fs_fuse_lookup(resolved, &nodeid, /*out_attr=*/NULL);
        free(resolved);
    }
    if (ret < 0)
        return ret;

    uint64_t size = *inout_size;
    switch (op) {
        case PAL_XATTR_GET:
        case PAL_XATTR_LIST:
            ret = virtio_fs_fuse_getxattr(nodeid, op == PAL_XATTR_GET ? name : NULL, value, &size);
            if (ret == 0)
                *inout_size = size;
            return ret;
        case PAL_XATTR_SET:
            return virtio_fs_fuse_setxattr(nodeid, name, value, size,
                                           flags & (PAL_XATTR_CREATE | PAL_XATTR_REPLACE));
        case PAL_XATTR_REMOVE:
            return virtio_fs_fuse_removexattr(nodeid, name);
        default:
            return -PAL_ERROR_INVAL;
    }
}

int pal_common_file_symlink(const char* target, const char* uri) {
    int ret;
    char* modified_path = NULL;
    char* dir_path = NULL;
    char* base_path = NULL;
    char* resolved_dir = NULL;
    ret = extract_dir_and_base(uri, &modified_path, &dir_path, &base_path);
    if (ret < 0)
        goto out;
    ret = realpath(dir_path, /*got_path=*/NULL, &resolved_dir);
    if (ret < 0)
        goto out;
    uint64_t dir_nodeid;
    ret = virtio_fs_fuse_lookup(resolved_dir, &dir_nodeid, /*out_attr=*/NULL);
    if (ret < 0)
        goto out;
    ret = virtio_fs_fuse_symlink(dir_nodeid, base_path, target);
out:
    free(modified_path);
    free(resolved_dir);
    return ret;
}

int pal_common_file_link(const char* old_uri, const char* new_uri) {
    int ret;
    char* old_modified = NULL;
    char* old_dir = NULL;
    char* old_base = NULL;
    char* old_resolved_dir = NULL;
    char* old_resolved_path = NULL;
    char* new_modified = NULL;
    char* new_dir = NULL;
    char* new_base = NULL;
    char* new_resolved_dir = NULL;

    ret = extract_dir_and_base(old_uri, &old_modified, &old_dir, &old_base);
    if (ret < 0)
        goto out;
    ret = realpath(old_dir, /*got_path=*/NULL, &old_resolved_dir);
    if (ret < 0)
        goto out;
    old_resolved_path = alloc_concat3(old_resolved_dir, -1, "/", 1, old_base, -1);
    if (!old_resolved_path) {
        ret = -PAL_ERROR_NOMEM;
        goto out;
    }
    uint64_t old_nodeid;
    ret = virtio_fs_fuse_lookup(old_resolved_path, &old_nodeid, /*out_attr=*/NULL);
    if (ret < 0)
        goto out;

    ret = extract_dir_and_base(new_uri, &new_modified, &new_dir, &new_base);
    if (ret < 0)
        goto out;
    ret = realpath(new_dir, /*got_path=*/NULL, &new_resolved_dir);
    if (ret < 0)
        goto out;
    uint64_t new_dir_nodeid;
    ret = virtio_fs_fuse_lookup(new_resolved_dir, &new_dir_nodeid, /*out_attr=*/NULL);
    if (ret < 0)
        goto out;
    ret = virtio_fs_fuse_link(old_nodeid, new_dir_nodeid, new_base);
out:
    free(old_modified);
    free(old_resolved_dir);
    free(old_resolved_path);
    free(new_modified);
    free(new_resolved_dir);
    return ret;
}

int pal_common_file_open(struct pal_handle** handle, const char* type, const char* uri,
                         enum pal_access access, pal_share_flags_t share,
                         enum pal_create_mode create, pal_stream_options_t options) {
    int ret;
    uint64_t nodeid;
    uint64_t fh;
    bool opened = false;
    struct pal_handle* hdl = NULL;
    char* uri_with_slash = NULL;
    char* norm_path = NULL;
    char* parent_resolved_path = NULL;
    char* resolved_path = NULL;

    /* for cleanup in case the file was created and there is a failure */
    uint64_t created_dir_nodeid = 0;
    char* created_base_path = NULL;

    //log_always("pal_common_file_open: type=%s, uri=%s, access=0x%x, share=0x%x, create=%d, options=0x%x",
    //            type, uri, access, share, create, options);

    if (strcmp(type, URI_TYPE_FILE))
        return -PAL_ERROR_INVAL;

    assert(create != PAL_CREATE_IGNORED);
    assert(WITHIN_MASK(share,   PAL_SHARE_MASK));
    assert(WITHIN_MASK(options, PAL_OPTION_MASK));

    bool file_exists;

    /* resolve symlinks in provided uri (virtiofs on the host doesn't do it, so we must) */
    ret = realpath(uri, /*got_path=*/NULL, &resolved_path);
    if (ret == -PAL_ERROR_STREAMNOTEXIST)
        file_exists = false;
    else if (ret >= 0)
        file_exists = true;
    else
        return ret;

    // Print the resolved path (debug)
    //log_always("Opening file: '%s' (resolved to '%s'), access=%d, share=%d, create=%d, options=0x%x",
    //            uri, resolved_path ? resolved_path : "(not exist)", access, share, create, options);

    if (!file_exists && create == PAL_CREATE_NEVER)
        return -PAL_ERROR_STREAMNOTEXIST;

    if (file_exists && create == PAL_CREATE_ALWAYS)
        return -PAL_ERROR_STREAMEXIST;

    if (!file_exists) {
        /* file doesn't exist and we should create it in corresponding dir */
        char* base_path = NULL;
        char* dir_path = NULL;
        ret = extract_dir_and_base(uri, &uri_with_slash, &dir_path, &base_path);
        if (ret < 0)
            goto out;

        ret = realpath(dir_path, /*got_path=*/NULL, &parent_resolved_path);
        if (ret < 0)
            goto out;

        uint64_t dir_nodeid;
        ret = virtio_fs_fuse_lookup(parent_resolved_path, &dir_nodeid, /*out_attr=*/NULL);
        if (ret < 0)
            goto out;

        ret = virtio_fs_fuse_create(dir_nodeid, base_path, PAL_ACCESS_TO_LINUX_OPEN(access)  |
                                                           PAL_CREATE_TO_LINUX_OPEN(create)  |
                                                           PAL_OPTION_TO_LINUX_OPEN(options) |
                                                           O_CLOEXEC,
                                    share, &nodeid, &fh);
        if (ret < 0)
            goto out;

        created_dir_nodeid = dir_nodeid;
        created_base_path = strdup(base_path);
    } else {
        /* file exists (and was already resolved), simply open it */
        ret = virtio_fs_fuse_lookup(resolved_path, &nodeid, /*out_attr=*/NULL);
        if (ret < 0)
            goto out;

        ret = virtio_fs_fuse_open(nodeid, PAL_ACCESS_TO_LINUX_OPEN(access)  |
                                          PAL_CREATE_TO_LINUX_OPEN(create)  |
                                          PAL_OPTION_TO_LINUX_OPEN(options) |
                                          O_CLOEXEC,
                                  &fh);
        if (ret < 0)
            goto out;
    }

    // log_always("Opened file");

    /* now that we opened the host file, need to create and populate corresponding PAL handle */
    opened = true;

    hdl = calloc(1, sizeof(*hdl));
    if (!hdl) {
        ret = -PAL_ERROR_NOMEM;
        goto out;
    }

    size_t norm_path_size = strlen(uri) + 1;
    norm_path = malloc(norm_path_size);
    if (!norm_path) {
        ret = -PAL_ERROR_NOMEM;
        goto out;
    }

    if (!get_norm_path(uri, norm_path, &norm_path_size)) {
        ret = -PAL_ERROR_INVAL;
        goto out;
    }

    init_handle_hdr(hdl, PAL_TYPE_FILE);
    hdl->flags |= PAL_HANDLE_FD_READABLE | PAL_HANDLE_FD_WRITABLE;

    hdl->file.nodeid   = nodeid;
    hdl->file.fh       = fh;
    hdl->file.realpath = norm_path;
    hdl->file.writable = access != PAL_ACCESS_RDONLY;

    struct trusted_file* tf = NULL;
    if (g_use_trusted_files && !(options & PAL_OPTION_PASSTHROUGH)) {
        tf = get_trusted_or_allowed_file(hdl->file.realpath);
        if (!tf) {
            if (get_file_check_policy() != FILE_CHECK_POLICY_ALLOW_ALL_BUT_LOG) {
                log_warning("Disallowing access to file '%s'; file is not trusted or allowed.",
                            hdl->file.realpath);
                ret = -PAL_ERROR_DENIED;
                goto out;
            }
            log_warning("Allowing access to unknown file '%s' due to file_check_policy settings.",
                        hdl->file.realpath);
        }
    }

    if (tf && !tf->allowed && (!file_exists
                || (access == PAL_ACCESS_RDWR)
                || (access == PAL_ACCESS_WRONLY))) {
        log_error("Disallowing create/write/append to a trusted file '%s'", hdl->file.realpath);
        ret = -PAL_ERROR_DENIED;
        goto out;
    }

    if (tf) {
        /* now we can learn the size of the trusted file */
        struct fuse_attr attr;
        ret = virtio_fs_fuse_getattr(hdl->file.nodeid, hdl->file.fh, FUSE_GETATTR_FH, UINT64_MAX,
                                     &attr);
        if (ret < 0)
            goto out;
        tf->size = attr.size;

        void* chunk_hashes = NULL;
        ret = load_trusted_or_allowed_file(tf, hdl, !file_exists, &chunk_hashes);
        if (ret < 0)
            goto out;

        hdl->file.chunk_hashes = chunk_hashes;
        hdl->file.size = tf->size;
    }

#ifdef __x86_64__
    shared_file_note_open(hdl->file.nodeid);
#endif
    *handle = hdl;
    ret = 0;
out:
    free(uri_with_slash);
    free(parent_resolved_path);
    free(resolved_path);
    if (ret < 0) {
        free(norm_path);
        free(hdl);
        if (opened)
            virtio_fs_fuse_release(nodeid, fh);
        if (created_dir_nodeid && created_base_path)
            virtio_fs_fuse_unlink(created_dir_nodeid, created_base_path);
    }
    free(created_base_path);
    return ret;
}

int64_t pal_common_file_read(struct pal_handle* handle, uint64_t offset, uint64_t count,
                             void* buffer) {
    int ret;

    if (!handle->file.chunk_hashes) {
        /* case of passthrough/allowed file */

        /* try to read the whole buffer (this is important for some workloads like Java); do it in
         * FILE_CHUNK_SIZE chunks because virtio-fs cannot consume more than this limit at a time */
        uint64_t total_read_size = 0;
        while (total_read_size < count) {
            uint64_t read_size;
            ret = virtio_fs_fuse_read(handle->file.nodeid, handle->file.fh,
                                      MIN(count - total_read_size, FILE_CHUNK_SIZE),
                                      offset + total_read_size, buffer + total_read_size,
                                      &read_size);
            if (ret < 0) {
                if (ret == -PAL_ERROR_INTERRUPTED)
                    continue;
                return total_read_size ? (int64_t)total_read_size : ret;
            }

            if (read_size == 0)
                break;

            total_read_size += read_size;
        }

#ifdef __x86_64__
        shared_file_sync_buffer(handle, offset, total_read_size, buffer, /*to_cache=*/false);
#endif
        return (int64_t)total_read_size;
    }

    /* case of trusted file */
    uint64_t file_size = handle->file.size;
    if (offset >= file_size)
        return 0;

    int64_t end = MIN(offset + count, file_size);
    int64_t aligned_offset = ALIGN_DOWN(offset, TRUSTED_CHUNK_SIZE);
    int64_t aligned_end    = ALIGN_UP(end, TRUSTED_CHUNK_SIZE);

    ret = copy_and_verify_trusted_file(handle, buffer, aligned_offset, aligned_end, offset, end,
                                       handle->file.chunk_hashes, file_size);
    if (ret < 0)
        return ret;

    return end - offset;
}

int64_t pal_common_file_write(struct pal_handle* handle, uint64_t offset, uint64_t count,
                              const void* buffer) {
    if (handle->file.chunk_hashes) {
        /* case of trusted file: disallow writing completely */
        log_warning("Writing to a trusted file (%s) is disallowed!", handle->file.realpath);
        return -PAL_ERROR_DENIED;
    }

    /* try to write the whole buffer (this is important for some workloads like Python3); do it in
     * FILE_CHUNK_SIZE chunks because virtio-fs cannot consume more than this limit at a time */
    uint64_t total_written_size = 0;
    while (total_written_size < count) {
        uint64_t written_size;
        int ret = virtio_fs_fuse_write(handle->file.nodeid, handle->file.fh,
                                       buffer + total_written_size,
                                       MIN(count - total_written_size, FILE_CHUNK_SIZE),
                                       offset + total_written_size, &written_size);
        if (ret < 0)
            return total_written_size ? (int64_t)total_written_size : ret;

        if (written_size == 0)
            break;

        total_written_size += written_size;
    }

#ifdef __x86_64__
    shared_file_sync_buffer(handle, offset, total_written_size, (void*)buffer, /*to_cache=*/true);
#endif
    return (int64_t)total_written_size;
}

void pal_common_file_destroy(struct pal_handle* handle) {
    assert(handle->hdr.type == PAL_TYPE_FILE);

#ifdef __x86_64__
    shared_file_note_close(handle);
#endif
    int ret = virtio_fs_fuse_release(handle->file.nodeid, handle->file.fh);
    if (ret < 0) {
        log_error("closing file host fd %lu failed: %s", handle->file.nodeid, pal_strerror(ret));
        /* We cannot do anything about it anyway... */
    }

    free(handle->file.realpath);
    free(handle);
}

int pal_common_file_map(struct pal_handle* handle, void* addr, pal_prot_flags_t prot,
                        uint64_t offset, uint64_t size) {
    int ret;

    assert(IS_ALLOC_ALIGNED(offset) && IS_ALLOC_ALIGNED(size));

#ifdef __x86_64__
    if (!(prot & PAL_PROT_WRITECOPY) && (prot & PAL_PROT_WRITE) && memory_frame_pool_active()
            && current_address_space() && !handle->file.chunk_hashes)
        return shared_file_map(handle, current_address_space(), addr, prot, offset, size);
#endif

    if (!(prot & PAL_PROT_WRITECOPY) && (prot & PAL_PROT_WRITE)) {
        log_warning("App tries to create a writable shared file mapping. This is impossible.");
        return -PAL_ERROR_DENIED;
    }

    /* note that we need to first mmap with write permission (to update the mem region with file
     * contents), and then we mprotect back to read-only (if was requested) */
    bool read  = !!(prot & PAL_PROT_READ);
    bool write = !!(prot & (PAL_PROT_WRITE | PAL_PROT_WRITECOPY));
    bool execute = !!(prot & PAL_PROT_EXEC);
#ifdef __x86_64__
    struct memory_address_space* address_space = current_address_space();
    ret = address_space
            ? memory_address_space_alloc(address_space, addr, size, read, /*write=*/true, execute)
            : memory_alloc(addr, size, read, /*write=*/true, execute);
#else
    ret = memory_alloc(addr, size, read, /*write=*/true, execute);
#endif
    if (ret < 0)
        return ret;

    if (!handle->file.chunk_hashes) {
        /* case of allowed file */
        ret = emulate_file_map_via_read(handle->file.nodeid, handle->file.fh, addr, offset, size);
        goto out;
    }

    /* case of trusted file */
    int64_t end = MIN(offset + size, handle->file.size);
    size_t bytes_filled;

    if ((int64_t)offset >= end) {
        /* file is mmapped at offset beyond file size, there are no trusted-file contents to back
         * mmapped enclave pages; this is a legit case, so simply zero out these enclave pages and
         * return success */
        bytes_filled = 0;
    } else {
        int64_t aligned_offset = ALIGN_DOWN(offset, TRUSTED_CHUNK_SIZE);
        int64_t aligned_end    = ALIGN_UP(end, TRUSTED_CHUNK_SIZE);

        ret = copy_and_verify_trusted_file(handle, addr, aligned_offset, aligned_end, offset, end,
                                           handle->file.chunk_hashes, handle->file.size);
        if (ret < 0) {
            log_error("Verification of trusted file failed during mmap: %s", pal_strerror(ret));
            goto out;
        }

        bytes_filled = end - offset;
    }

    if (size > bytes_filled) {
        /* file ended before all mmapped memory was filled -- remaining memory must be zeroed */
        memset((char*)addr + bytes_filled, 0, size - bytes_filled);
    }

    if (!write) {
        /* restore read-only permission */
#ifdef __x86_64__
        ret = address_space
                ? memory_address_space_protect(address_space, addr, size, read,
                                               /*write=*/false, execute)
                : memory_protect(addr, size, read, /*write=*/false, execute);
#else
        ret = memory_protect(addr, size, read, /*write=*/false, execute);
#endif
        if (ret < 0) {
            log_error("Cannot restore read-only permission during file mmap, fatal.");
            BUG();
        }
    }

    ret = 0;
out:
    if (ret < 0)
#ifdef __x86_64__
        if (address_space)
            (void)memory_address_space_free(address_space, addr, size);
        else
            (void)memory_free(addr, size);
#else
        (void)memory_free(addr, size);
#endif
    return ret;
}

int pal_common_file_setlength(struct pal_handle* handle, uint64_t length) {
    struct fuse_setattr_in setattr = { .valid = FATTR_FH | FATTR_SIZE,
                                       .fh    = handle->file.fh,
                                       .size  = length };
    return virtio_fs_fuse_setattr(handle->file.nodeid, &setattr);
}

int pal_common_file_flush(struct pal_handle* handle) {
    return virtio_fs_fuse_flush(handle->file.nodeid, handle->file.fh);
}

int pal_common_file_attrquerybyhdl(struct pal_handle* handle, PAL_STREAM_ATTR* pal_attr) {
    int ret;

    struct fuse_attr attr;
    ret = virtio_fs_fuse_getattr(handle->file.nodeid, handle->file.fh, FUSE_GETATTR_FH, UINT64_MAX,
                                 &attr);
    if (ret < 0)
        return ret;

    pal_attr->handle_type  = S_ISREG(attr.mode) ? PAL_TYPE_FILE : PAL_TYPE_DIR;
    pal_attr->share_flags  = attr.mode & PAL_SHARE_MASK;
    pal_attr->pending_size = attr.size;
    pal_attr->nonblocking  = false;
    return 0;
}

int pal_common_file_attrquery(const char* type, const char* uri, PAL_STREAM_ATTR* pal_attr) {
    if (strcmp(type, URI_TYPE_FILE) && strcmp(type, URI_TYPE_DIR))
        return -PAL_ERROR_INVAL;

    int ret;

    struct pal_handle* hdl = NULL;
    ret = pal_common_file_open(&hdl, type, uri, PAL_ACCESS_RDONLY, /*share_flags=*/0,
                               PAL_CREATE_NEVER, PAL_OPTION_PASSTHROUGH);
    if (ret < 0)
        return ret;

    ret = pal_common_file_attrquerybyhdl(hdl, pal_attr);

    pal_common_file_destroy(hdl);
    return ret;
}

int pal_common_file_attrsetbyhdl(struct pal_handle* handle, PAL_STREAM_ATTR* attr) {
    struct fuse_setattr_in setattr = { .valid = FATTR_FH, .fh = handle->file.fh };
    if (attr->set_atime) {
        if (attr->atime_now) {
            setattr.valid |= FATTR_ATIME_NOW;
        } else {
            setattr.valid |= FATTR_ATIME;
            setattr.atime = attr->atime.tv_sec;
            setattr.atimensec = attr->atime.tv_nsec;
        }
    }
    if (attr->set_mtime) {
        if (attr->mtime_now) {
            setattr.valid |= FATTR_MTIME_NOW;
        } else {
            setattr.valid |= FATTR_MTIME;
            setattr.mtime = attr->mtime.tv_sec;
            setattr.mtimensec = attr->mtime.tv_nsec;
        }
    }
    if (!attr->set_atime && !attr->set_mtime) {
        setattr.valid |= FATTR_MODE;
        setattr.mode = attr->share_flags;
    }
    return virtio_fs_fuse_setattr(handle->file.nodeid, &setattr);
}

int pal_common_file_delete(struct pal_handle* handle, enum pal_delete_mode delete_mode) {
    int ret;
    char* realpath_with_slash = NULL;
    char* resolved_path = NULL;

    if (delete_mode != PAL_DELETE_ALL)
        return -PAL_ERROR_INVAL;

    char* base_path = NULL;
    char* dir_path = NULL;
    ret = extract_dir_and_base(handle->file.realpath, &realpath_with_slash, &dir_path, &base_path);
    if (ret < 0)
        goto out;

    ret = realpath(dir_path, /*got_path=*/NULL, &resolved_path);
    if (ret < 0)
        goto out;

    uint64_t dir_nodeid;
    ret = virtio_fs_fuse_lookup(resolved_path, &dir_nodeid, /*out_attr=*/NULL);
    if (ret < 0)
        goto out;

    ret = virtio_fs_fuse_unlink(dir_nodeid, base_path);
    if (ret < 0)
        goto out;

    ret = 0;
out:
    free(realpath_with_slash);
    free(resolved_path);
    return ret;
}

int pal_common_file_rename(struct pal_handle* handle, const char* type, const char* uri) {
    if (strcmp(type, URI_TYPE_FILE))
        return -PAL_ERROR_INVAL;

    char* tmp = strdup(uri);
    if (!tmp)
        return -PAL_ERROR_NOMEM;

    int ret = file_or_dir_rename(handle->file.realpath, uri, /*flags=*/0);
    if (ret < 0) {
        free(tmp);
        return ret;
    }

    free(handle->file.realpath);
    handle->file.realpath = tmp;
    return 0;
}

int pal_common_dir_open(struct pal_handle** handle, const char* type, const char* uri,
                        enum pal_access access, pal_share_flags_t share,
                        enum pal_create_mode create, pal_stream_options_t options) {
    __UNUSED(access);

    int ret;
    uint64_t nodeid;
    uint64_t fh;
    bool opened = false;
    struct pal_handle* hdl = NULL;
    char* uri_with_slash = NULL;
    char* norm_path = NULL;
    char* parent_resolved_path = NULL;
    char* resolved_path = NULL;

    /* for cleanup in case the dir was created and there is a failure */
    uint64_t created_dir_nodeid = 0;
    char* created_base_path = NULL;

    if (strcmp(type, URI_TYPE_DIR))
        return -PAL_ERROR_INVAL;

    assert(create != PAL_CREATE_IGNORED);

    bool dir_exists;

    /* resolve symlinks in provided uri (virtiofs on the host doesn't do it, so we must) */
    ret = realpath(uri, /*got_path=*/NULL, &resolved_path);
    if (ret == -PAL_ERROR_STREAMNOTEXIST)
        dir_exists = false;
    else if (ret >= 0)
        dir_exists = true;
    else
        return ret;

    if (!dir_exists && create == PAL_CREATE_NEVER)
        return -PAL_ERROR_STREAMNOTEXIST;

    if (dir_exists && create == PAL_CREATE_ALWAYS)
        return -PAL_ERROR_STREAMEXIST;

    if (!dir_exists) {
        /* dir doesn't exist and we should create it in corresponding parent dir */
        char* base_path = NULL;
        char* dir_path = NULL;
        ret = extract_dir_and_base(uri, &uri_with_slash, &dir_path, &base_path);
        if (ret < 0)
            goto out;

        ret = realpath(dir_path, /*got_path=*/NULL, &parent_resolved_path);
        if (ret < 0)
            goto out;

        uint64_t parent_dir_nodeid;
        ret = virtio_fs_fuse_lookup(parent_resolved_path, &parent_dir_nodeid, /*out_attr=*/NULL);
        if (ret < 0)
            goto out;

        ret = virtio_fs_fuse_mkdir(parent_dir_nodeid, base_path, share, &nodeid);
        if (ret < 0)
            goto out;

        created_dir_nodeid = parent_dir_nodeid;
        created_base_path = strdup(base_path);
    } else {
        /* dir exists (and was already resolved), simply look it up */
        ret = virtio_fs_fuse_lookup(resolved_path, &nodeid, /*out_attr=*/NULL);
        if (ret < 0)
            goto out;
    }

    ret = virtio_fs_fuse_opendir(nodeid, PAL_OPTION_TO_LINUX_OPEN(options) | O_CLOEXEC, &fh);
    if (ret < 0)
        goto out;

    /* now that we opened the host dir, need to create and populate corresponding PAL handle */
    opened = true;

    hdl = calloc(1, sizeof(*hdl));
    if (!hdl) {
        ret = -PAL_ERROR_NOMEM;
        goto out;
    }

    size_t norm_path_size = strlen(uri) + 1;
    norm_path = malloc(norm_path_size);
    if (!norm_path) {
        ret = -PAL_ERROR_NOMEM;
        goto out;
    }

    if (!get_norm_path(uri, norm_path, &norm_path_size)) {
        ret = -PAL_ERROR_INVAL;
        goto out;
    }

    init_handle_hdr(hdl, PAL_TYPE_DIR);
    hdl->flags |= PAL_HANDLE_FD_READABLE;

    hdl->dir.nodeid   = nodeid;
    hdl->dir.fh       = fh;
    hdl->dir.realpath = norm_path;

    hdl->dir.buf         = NULL;
    hdl->dir.ptr         = NULL;
    hdl->dir.end         = NULL;
    hdl->dir.endofstream = false;

    *handle = hdl;
    ret = 0;
out:
    free(uri_with_slash);
    free(parent_resolved_path);
    free(resolved_path);
    if (ret < 0) {
        free(norm_path);
        free(hdl);
        if (opened)
            virtio_fs_fuse_releasedir(nodeid, fh);
        if (created_dir_nodeid && created_base_path)
            virtio_fs_fuse_rmdir(created_dir_nodeid, created_base_path);
    }
    free(created_base_path);
    return ret;
}

int64_t pal_common_dir_read(struct pal_handle* handle, uint64_t offset, size_t count, void* _buf) {
    int ret;
    size_t bytes_written = 0;
    char* buf = (char*)_buf;

    uint64_t last_fuse_dirent_off = 0;

    if (offset)
        return -PAL_ERROR_INVAL;

    if (handle->dir.endofstream)
        return 0;

    while (1) {
        while ((char*)handle->dir.ptr < (char*)handle->dir.end) {
            struct fuse_dirent* dirent = (struct fuse_dirent*)handle->dir.ptr;

            if (is_dot_or_dotdot(dirent->name))
                goto skip;

            bool is_dir = dirent->type == DT_DIR;
            size_t len = dirent->namelen;
            if ((ssize_t)len >= (char*)handle->dir.end - (char*)handle->dir.ptr)
                return -PAL_ERROR_DENIED;

            if (len + 1 + (is_dir ? 1 : 0) > count)
                goto out;

            memcpy(buf, dirent->name, len);
            if (is_dir)
                buf[len++] = '/';
            buf[len++] = '\0';

            buf += len;
            bytes_written += len;
            count -= len;
        skip:
            handle->dir.ptr = (char*)handle->dir.ptr + FUSE_DIRENT_SIZE(dirent);

            /* fuse_dirent:off contains a unique number (not really an offset) that identifies the
             * last read entry; this off must be specified in the next FUSE READDIR request; see
             * gitlab.com/virtio-fs/virtiofsd/-/blob/v1.6.0/src/filesystem.rs?ref_type=tags#L1000 */
            last_fuse_dirent_off = dirent->off;
        }

        if (!count) {
            /* No space left, returning */
            goto out;
        }

        if (!handle->dir.buf) {
            handle->dir.buf = malloc(DIRBUF_SIZE);
            if (!handle->dir.buf)
                return -PAL_ERROR_NOMEM;
        }

        uint64_t size;
        ret = virtio_fs_fuse_readdir(handle->dir.nodeid, handle->dir.fh, DIRBUF_SIZE,
                                     /*offset=*/last_fuse_dirent_off, handle->dir.buf, &size);
        if (ret < 0) {
            if (bytes_written) {
                /* If something was written just return that and pretend no error was seen - it will
                 * be caught next time. */
                goto out;
            }
            return ret;
        }

        if (!size) {
            handle->dir.endofstream = true;
            goto out;
        }

        handle->dir.ptr = handle->dir.buf;
        handle->dir.end = (char*)handle->dir.buf + size;
    }

out:
    return (int64_t)bytes_written;
}

void pal_common_dir_destroy(struct pal_handle* handle) {
    assert(handle->hdr.type == PAL_TYPE_DIR);

    int ret = virtio_fs_fuse_releasedir(handle->dir.nodeid, handle->dir.fh);
    if (ret < 0) {
        log_error("closing dir host fd %lu failed: %s", handle->dir.nodeid, pal_strerror(ret));
        /* We cannot do anything about it anyway... */
    }

    free(handle->dir.buf);
    free(handle->dir.realpath);
    free(handle);
}

int pal_common_dir_delete(struct pal_handle* handle, enum pal_delete_mode delete_mode) {
    int ret;
    char* realpath_with_slash = NULL;
    char* resolved_path = NULL;

    if (delete_mode != PAL_DELETE_ALL)
        return -PAL_ERROR_INVAL;

    char* base_path = NULL;
    char* dir_path = NULL;
    ret = extract_dir_and_base(handle->dir.realpath, &realpath_with_slash, &dir_path, &base_path);
    if (ret < 0)
        goto out;

    ret = realpath(dir_path, /*got_path=*/NULL, &resolved_path);
    if (ret < 0)
        goto out;

    uint64_t parent_dir_nodeid;
    ret = virtio_fs_fuse_lookup(resolved_path, &parent_dir_nodeid, /*out_attr=*/NULL);
    if (ret < 0)
        goto out;

    ret = virtio_fs_fuse_rmdir(parent_dir_nodeid, base_path);
    if (ret < 0)
        goto out;

    ret = 0;
out:
    free(realpath_with_slash);
    free(resolved_path);
    return ret;
}

int pal_common_dir_rename(struct pal_handle* handle, const char* type, const char* uri) {
    if (strcmp(type, URI_TYPE_DIR))
        return -PAL_ERROR_INVAL;

    char* tmp = strdup(uri);
    if (!tmp)
        return -PAL_ERROR_NOMEM;

    int ret = file_or_dir_rename(handle->dir.realpath, uri, /*flags=*/0);
    if (ret < 0) {
        free(tmp);
        return ret;
    }

    free(handle->dir.realpath);
    handle->dir.realpath = tmp;
    return 0;
}
