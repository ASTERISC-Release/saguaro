/* SPDX-License-Identifier: LGPL-3.0-or-later */
#include "libos_internal.h"
#include "libos_thread.h"
#include "libos_lock.h"
#include "linux_abi/errors.h"
#include "libos_table.h"

#define LIBOS_CAP_VERSION_1 0x19980330U
#define LIBOS_CAP_VERSION_2 0x20071026U
#define LIBOS_CAP_VERSION_3 0x20080522U
#define LIBOS_CAP_WORDS 2

struct libos_cap_header {
    uint32_t version;
    int32_t pid;
};
struct libos_cap_data {
    uint32_t effective;
    uint32_t permitted;
    uint32_t inheritable;
};

static size_t cap_words(uint32_t version) {
    switch (version) {
        case LIBOS_CAP_VERSION_1: return 1;
        case LIBOS_CAP_VERSION_2:
        case LIBOS_CAP_VERSION_3: return LIBOS_CAP_WORDS;
        default: return 0;
    }
}

long libos_syscall_capget(void* header_ptr, void* data_ptr) {
    struct libos_cap_header* header = header_ptr;
    struct libos_cap_data* data = data_ptr;
    if (!is_user_memory_readable(header, sizeof(*header)) ||
        !is_user_memory_writable(header, sizeof(*header)))
        return -EFAULT;
    size_t words = cap_words(header->version);
    if (!words) {
        header->version = LIBOS_CAP_VERSION_3;
        return -EINVAL;
    }
    if (header->pid < 0)
        return -EINVAL;
    if (header->pid && (IDTYPE)header->pid != get_cur_thread()->tid)
        return -ESRCH;
    if (!is_user_memory_writable(data, words * sizeof(*data)))
        return -EFAULT;
    struct libos_thread* t = get_cur_thread();
    lock(&t->lock);
    for (size_t i = 0; i < words; i++) {
        unsigned int shift = i * 32;
        data[i].effective = (uint32_t)(t->cap_effective >> shift);
        data[i].permitted = (uint32_t)(t->cap_permitted >> shift);
        data[i].inheritable = (uint32_t)(t->cap_inheritable >> shift);
    }
    unlock(&t->lock);
    return 0;
}

long libos_syscall_capset(void* header_ptr, const void* data_ptr) {
    struct libos_cap_header* header = header_ptr;
    const struct libos_cap_data* data = data_ptr;
    if (!is_user_memory_readable(header, sizeof(*header)))
        return -EFAULT;
    size_t words = cap_words(header->version);
    if (!words) {
        header->version = LIBOS_CAP_VERSION_3;
        return -EINVAL;
    }
    if (header->pid < 0)
        return -EINVAL;
    if (header->pid && (IDTYPE)header->pid != get_cur_thread()->tid)
        return -EPERM;
    if (!is_user_memory_readable(data, words * sizeof(*data)))
        return -EFAULT;
    uint64_t effective = 0, permitted = 0, inheritable = 0;
    for (size_t i = 0; i < words; i++) {
        unsigned int shift = i * 32;
        effective |= (uint64_t)data[i].effective << shift;
        permitted |= (uint64_t)data[i].permitted << shift;
        inheritable |= (uint64_t)data[i].inheritable << shift;
    }
    struct libos_thread* t = get_cur_thread();
    lock(&t->lock);
    if ((effective & ~permitted) || (permitted & ~t->cap_permitted) ||
        (inheritable & ~(t->cap_inheritable | t->cap_permitted))) {
        unlock(&t->lock);
        return -EPERM;
    }
    t->cap_effective = effective;
    t->cap_permitted = permitted;
    t->cap_inheritable = inheritable;
    unlock(&t->lock);
    return 0;
}
