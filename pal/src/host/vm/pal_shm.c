/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/*
 * POSIX shared memory of the guest (`dev:/dev/shm/<name>`, the `untrusted_shm` mount) in guest RAM,
 * with the experimental multiprocess support (x86_64).
 *
 * An object is a named, reference-counted set of frame-pool frames in a table in PAL memory, which
 * every process maps at the same address, so all processes of the guest see the same objects; no
 * host file is involved. The store holds one reference on each frame and every mapping leaf holds
 * another, so an unlinked object's frames stay while they are mapped, as tmpfs pages do. The
 * object itself goes with its name and its last handle. All store pages together are limited to
 * half of the frame pool.
 */

#include "api.h"
#include "pal.h"
#include "pal_common.h"
#include "pal_error.h"
#include "pal_internal.h"
#include "spinlock.h"

#include "kernel_memory.h"
#include "kernel_thread.h"

#define SHM_PREFIX "/dev/shm/"

struct shm_object {
    struct shm_object* next; /* in g_shm_objects while it has a name */
    char* name;              /* NULL once unlinked */
    pal_share_flags_t perm;
    uint64_t size;
    size_t frames_count;
    uintptr_t* frames; /* 0: page never touched (reads as zeros) */
    uint32_t handles;
};

static struct shm_object* g_shm_objects;
static size_t g_shm_frames_used;
static spinlock_t g_shm_lock = INIT_SPINLOCK_UNLOCKED;

bool vm_shm_store_active(void) {
    return memory_frame_pool_active();
}

bool vm_shm_is_object_uri(const char* uri) {
    return strstartswith(uri, SHM_PREFIX) && uri[strlen(SHM_PREFIX)] != '\0';
}

static struct shm_object* shm_find(const char* name) {
    assert(spinlock_is_locked(&g_shm_lock));
    for (struct shm_object* obj = g_shm_objects; obj; obj = obj->next)
        if (!strcmp(obj->name, name))
            return obj;
    return NULL;
}

static void shm_unlink_locked(struct shm_object* obj) {
    assert(spinlock_is_locked(&g_shm_lock));
    struct shm_object** link = &g_shm_objects;
    while (*link && *link != obj)
        link = &(*link)->next;
    if (*link)
        *link = obj->next;
    obj->next = NULL;
    free(obj->name);
    obj->name = NULL;
}

/* Drops the store's references on the pages from `first_page` on (mapped frames stay alive). */
static void shm_release_pages(struct shm_object* obj, size_t first_page) {
    assert(spinlock_is_locked(&g_shm_lock));
    for (size_t i = first_page; i < obj->frames_count; i++) {
        if (obj->frames[i]) {
            memory_frame_unref(obj->frames[i]);
            obj->frames[i] = 0;
            g_shm_frames_used--;
        }
    }
}

static void shm_maybe_free(struct shm_object* obj) {
    assert(spinlock_is_locked(&g_shm_lock));
    if (obj->name || obj->handles)
        return;
    shm_release_pages(obj, 0);
    free(obj->frames);
    free(obj);
}

static int shm_grow_frames(struct shm_object* obj, size_t pages) {
    if (pages <= obj->frames_count)
        return 0;
    size_t new_count = MAX(pages, obj->frames_count * 2);
    uintptr_t* frames = calloc(new_count, sizeof(*frames));
    if (!frames)
        return -PAL_ERROR_NOMEM;
    if (obj->frames_count)
        memcpy(frames, obj->frames, obj->frames_count * sizeof(*frames));
    free(obj->frames);
    obj->frames = frames;
    obj->frames_count = new_count;
    return 0;
}

/* Returns the frame of page `page` with an extra reference for the caller (who drops it with
 * memory_frame_unref()), allocating a zero frame for a page never touched. */
static int shm_get_page(struct shm_object* obj, size_t page, uintptr_t* out_frame) {
    assert(spinlock_is_locked(&g_shm_lock));
    int ret = shm_grow_frames(obj, page + 1);
    if (ret < 0)
        return ret;
    if (!obj->frames[page]) {
        if (g_shm_frames_used >= memory_frame_pool_capacity() / 2)
            return -PAL_ERROR_NOMEM;
        uintptr_t frame;
        ret = memory_frame_alloc(&frame);
        if (ret < 0)
            return ret;
        obj->frames[page] = frame;
        g_shm_frames_used++;
    }
    memory_frame_ref(obj->frames[page]);
    *out_frame = obj->frames[page];
    return 0;
}

int vm_shm_open(PAL_HANDLE* out_handle, const char* uri, enum pal_access access,
                pal_share_flags_t share, enum pal_create_mode create) {
    const char* name = uri + strlen(SHM_PREFIX);
    if (strchr(name, '/'))
        return -PAL_ERROR_DENIED; /* no subdirectories, as the LibOS `shm` file system says */

    struct pal_handle* handle = calloc(1, HANDLE_SIZE(shm));
    if (!handle)
        return -PAL_ERROR_NOMEM;
    char* name_copy = strdup(name);
    struct shm_object* new_obj = calloc(1, sizeof(*new_obj));
    if (!name_copy || !new_obj) {
        free(handle);
        free(name_copy);
        free(new_obj);
        return -PAL_ERROR_NOMEM;
    }

    spinlock_lock(&g_shm_lock);
    struct shm_object* obj = shm_find(name);
    int ret = 0;
    if (obj && create == PAL_CREATE_ALWAYS) {
        ret = -PAL_ERROR_STREAMEXIST;
    } else if (!obj && create == PAL_CREATE_NEVER) {
        ret = -PAL_ERROR_STREAMNOTEXIST;
    } else if (!obj) {
        obj = new_obj;
        new_obj = NULL;
        obj->name = name_copy;
        name_copy = NULL;
        obj->perm = share;
        obj->next = g_shm_objects;
        g_shm_objects = obj;
    }
    if (ret == 0)
        obj->handles++;
    spinlock_unlock(&g_shm_lock);

    free(name_copy);
    free(new_obj);
    if (ret < 0) {
        free(handle);
        return ret;
    }
    init_handle_hdr(handle, PAL_TYPE_DEV);
    handle->shm.writable = access != PAL_ACCESS_RDONLY;
    handle->shm.object = obj;
    *out_handle = handle;
    return 0;
}

bool vm_shm_is_handle(PAL_HANDLE handle) {
    return handle->hdr.type == PAL_TYPE_DEV && handle->shm.object;
}

void vm_shm_destroy(PAL_HANDLE handle) {
    struct shm_object* obj = handle->shm.object;
    spinlock_lock(&g_shm_lock);
    assert(obj->handles > 0);
    obj->handles--;
    shm_maybe_free(obj);
    spinlock_unlock(&g_shm_lock);
    free(handle);
}

int vm_shm_delete(PAL_HANDLE handle) {
    struct shm_object* obj = handle->shm.object;
    spinlock_lock(&g_shm_lock);
    if (obj->name)
        shm_unlink_locked(obj);
    spinlock_unlock(&g_shm_lock);
    /* the object itself stays until this handle is destroyed */
    return 0;
}

int vm_shm_setlength(PAL_HANDLE handle, uint64_t length) {
    if (!handle->shm.writable)
        return -PAL_ERROR_DENIED;
    struct shm_object* obj = handle->shm.object;
    spinlock_lock(&g_shm_lock);
    if (length < obj->size)
        shm_release_pages(obj, ALIGN_UP(length, PAGE_SIZE) / PAGE_SIZE);
    obj->size = length;
    spinlock_unlock(&g_shm_lock);
    return 0;
}

int vm_shm_attrquerybyhdl(PAL_HANDLE handle, PAL_STREAM_ATTR* attr) {
    struct shm_object* obj = handle->shm.object;
    memset(attr, 0, sizeof(*attr));
    spinlock_lock(&g_shm_lock);
    attr->handle_type  = PAL_TYPE_FILE; /* the LibOS `shm` file system takes files as devices */
    attr->share_flags  = obj->perm;
    attr->pending_size = obj->size;
    spinlock_unlock(&g_shm_lock);
    return 0;
}

int vm_shm_attrquery(const char* uri, PAL_STREAM_ATTR* attr) {
    memset(attr, 0, sizeof(*attr));
    if (!strcmp(uri, "/dev/shm") || !strcmp(uri, SHM_PREFIX)) {
        attr->handle_type = PAL_TYPE_DIR;
        attr->share_flags = 01777;
        return 0;
    }
    const char* name = uri + strlen(SHM_PREFIX);
    spinlock_lock(&g_shm_lock);
    struct shm_object* obj = shm_find(name);
    if (obj) {
        attr->handle_type  = PAL_TYPE_FILE;
        attr->share_flags  = obj->perm;
        attr->pending_size = obj->size;
    }
    spinlock_unlock(&g_shm_lock);
    return obj ? 0 : -PAL_ERROR_STREAMNOTEXIST;
}

static struct memory_address_space* current_address_space(void) {
    struct pal_tcb_vm* tcb = (struct pal_tcb_vm*)pal_get_tcb();
    return tcb ? tcb->kernel_thread.address_space : NULL;
}

int vm_shm_map(PAL_HANDLE handle, void* addr, pal_prot_flags_t prot, uint64_t offset,
               uint64_t size) {
    struct shm_object* obj = handle->shm.object;
    struct memory_address_space* space = current_address_space();
    if (!space)
        return -PAL_ERROR_NOTSUPPORT;
    bool write = !!(prot & PAL_PROT_WRITE);
    bool execute = !!(prot & PAL_PROT_EXEC);
    if (write && !(prot & PAL_PROT_WRITECOPY) && !handle->shm.writable)
        return -PAL_ERROR_DENIED;
    if (prot & PAL_PROT_WRITECOPY) {
        /* a private mapping: a copy of the current contents */
        int ret = memory_address_space_alloc(space, addr, size, /*read=*/true, /*write=*/true,
                                             execute);
        if (ret < 0)
            return ret;
        for (uint64_t done = 0; done < size; done += PAGE_SIZE) {
            size_t page = (offset + done) / PAGE_SIZE;
            spinlock_lock(&g_shm_lock);
            uintptr_t frame = page < obj->frames_count ? obj->frames[page] : 0;
            if (frame)
                memory_frame_ref(frame);
            spinlock_unlock(&g_shm_lock);
            if (frame) {
                memcpy((char*)addr + done, memory_frame_pointer(frame), PAGE_SIZE);
                memory_frame_unref(frame);
            }
        }
        if (!(prot & PAL_PROT_WRITE) || !(prot & PAL_PROT_READ))
            return memory_address_space_protect(space, addr, size, !!(prot & PAL_PROT_READ),
                                                !!(prot & PAL_PROT_WRITE), execute);
        return 0;
    }

    for (uint64_t done = 0; done < size; done += PAGE_SIZE) {
        uintptr_t frame;
        spinlock_lock(&g_shm_lock);
        int ret = shm_get_page(obj, (offset + done) / PAGE_SIZE, &frame);
        spinlock_unlock(&g_shm_lock);
        if (ret == 0) {
            ret = memory_address_space_map_shared_frame(space, (uintptr_t)addr + done, frame,
                                                        write, execute);
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

static int64_t shm_copy(PAL_HANDLE handle, uint64_t offset, uint64_t count, void* buffer,
                        bool to_object) {
    struct shm_object* obj = handle->shm.object;
    if (to_object && !handle->shm.writable)
        return -PAL_ERROR_DENIED;
    spinlock_lock(&g_shm_lock);
    if (to_object && offset + count > obj->size)
        obj->size = offset + count;
    uint64_t end = MIN(offset + count, obj->size);
    spinlock_unlock(&g_shm_lock);
    if (offset >= end)
        return 0;

    for (uint64_t pos = offset; pos < end;) {
        size_t page = pos / PAGE_SIZE;
        size_t in_page = pos % PAGE_SIZE;
        size_t chunk = MIN(PAGE_SIZE - in_page, end - pos);
        uintptr_t frame = 0;
        int ret = 0;
        spinlock_lock(&g_shm_lock);
        if (to_object) {
            ret = shm_get_page(obj, page, &frame);
        } else if (page < obj->frames_count && obj->frames[page]) {
            frame = obj->frames[page];
            memory_frame_ref(frame);
        }
        spinlock_unlock(&g_shm_lock);
        if (ret < 0)
            return pos > offset ? (int64_t)(pos - offset) : ret;
        char* buf = (char*)buffer + (pos - offset);
        if (to_object)
            memcpy((char*)memory_frame_pointer(frame) + in_page, buf, chunk);
        else if (frame)
            memcpy(buf, (char*)memory_frame_pointer(frame) + in_page, chunk);
        else
            memset(buf, 0, chunk);
        if (frame)
            memory_frame_unref(frame);
        pos += chunk;
    }
    return (int64_t)(end - offset);
}

int64_t vm_shm_read(PAL_HANDLE handle, uint64_t offset, uint64_t count, void* buffer) {
    return shm_copy(handle, offset, count, buffer, /*to_object=*/false);
}

int64_t vm_shm_write(PAL_HANDLE handle, uint64_t offset, uint64_t count, const void* buffer) {
    return shm_copy(handle, offset, count, (void*)buffer, /*to_object=*/true);
}
