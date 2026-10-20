/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/*
 * set_thread_area() and get_thread_area(): Linux's three per-thread GDT TLS slots (entries 12 to
 * 14, selectors 0x63, 0x6b, 0x73), kept per thread by the PAL (PalSegmentDescriptorSet(): the VM
 * PAL implements them, the others answer NOTIMPLEMENTED, reported as ENOSYS as before). The
 * `user_desc` is encoded into a raw descriptor exactly as Linux's fill_ldt() does. Wine's new
 * WoW64 allocates its 32-bit %fs selector here, through `int $0x80`, which the VM kernel turns
 * into this system call (dev docs: proton-vm/design-32bit.md). A new thread starts
 * with empty slots (Linux copies the parent's); modify_ldt() stays unimplemented.
 */

#include "libos_internal.h"
#include "libos_table.h"
#include "linux_abi/errors.h"
#include "pal.h"

#define GDT_ENTRY_TLS_MIN 12
#define GDT_ENTRY_TLS_MAX 14

/* <asm/ldt.h> */
struct user_desc {
    unsigned int entry_number;
    unsigned int base_addr;
    unsigned int limit;
    unsigned int seg_32bit : 1;
    unsigned int contents : 2;
    unsigned int read_exec_only : 1;
    unsigned int limit_in_pages : 1;
    unsigned int seg_not_present : 1;
    unsigned int useable : 1;
    unsigned int lm : 1;
};

/* Linux's LDT_empty(): the "empty" descriptor glibc and Wine pass to free a slot */
static bool user_desc_empty(const struct user_desc* info) {
    return info->base_addr == 0 && info->limit == 0 && info->contents == 0 &&
           info->read_exec_only == 1 && info->seg_32bit == 0 && info->limit_in_pages == 0 &&
           info->seg_not_present == 1 && info->useable == 0;
}

/* Linux's LDT_zero() */
static bool user_desc_zero(const struct user_desc* info) {
    return info->base_addr == 0 && info->limit == 0 && info->contents == 0 &&
           info->read_exec_only == 0 && info->seg_32bit == 0 && info->limit_in_pages == 0 &&
           info->seg_not_present == 0 && info->useable == 0;
}

/* Linux's fill_ldt(): a user (DPL 3) segment; the accessed bit set, the L bit never */
static uint64_t user_desc_encode(const struct user_desc* info) {
    uint64_t type = ((info->read_exec_only ^ 1) << 1) | (info->contents << 2) | 1;
    uint64_t desc = 0;
    desc |= (uint64_t)(info->limit & 0xffff);
    desc |= (uint64_t)(info->base_addr & 0xffffff) << 16;
    desc |= type << 40;
    desc |= 1ULL << 44;                                      /* S: code or data */
    desc |= 3ULL << 45;                                      /* DPL 3 */
    desc |= (uint64_t)(info->seg_not_present ^ 1) << 47;     /* P */
    desc |= (uint64_t)((info->limit >> 16) & 0xf) << 48;
    desc |= (uint64_t)info->useable << 52;                   /* AVL */
    desc |= (uint64_t)info->seg_32bit << 54;                 /* D/B */
    desc |= (uint64_t)info->limit_in_pages << 55;            /* G */
    desc |= (uint64_t)((info->base_addr >> 24) & 0xff) << 56;
    return desc;
}

/* Linux's fill_user_desc() */
static void user_desc_decode(uint64_t desc, unsigned int entry, struct user_desc* info) {
    memset(info, 0, sizeof(*info));
    info->entry_number    = entry;
    info->base_addr       = ((desc >> 16) & 0xffffff) | (((desc >> 56) & 0xff) << 24);
    info->limit           = (desc & 0xffff) | (((desc >> 48) & 0xf) << 16);
    unsigned int type     = (desc >> 40) & 0xf;
    info->seg_32bit       = (desc >> 54) & 1;
    info->contents        = (type >> 2) & 3;
    info->read_exec_only  = !((type >> 1) & 1);
    info->limit_in_pages  = (desc >> 55) & 1;
    info->seg_not_present = !((desc >> 47) & 1);
    info->useable         = (desc >> 52) & 1;
    info->lm              = (desc >> 53) & 1;
}

long libos_syscall_set_thread_area(void* u_info) {
    struct user_desc* user_info = u_info;
    struct user_desc info;
    if (!is_user_memory_readable(user_info, sizeof(info)))
        return -EFAULT;
    memcpy(&info, user_info, sizeof(info));

    int idx = (int)info.entry_number;
    if (idx == -1) {
        /* the first empty slot of this thread; ESRCH when all three are in use */
        for (idx = GDT_ENTRY_TLS_MIN; idx <= GDT_ENTRY_TLS_MAX; idx++) {
            uint64_t current;
            int ret = PalSegmentDescriptorGet(idx, &current);
            if (ret < 0)
                return pal_to_unix_errno(ret);
            if (current == 0)
                break;
        }
        if (idx > GDT_ENTRY_TLS_MAX)
            return -ESRCH;
        if (!is_user_memory_writable(&user_info->entry_number, sizeof(user_info->entry_number)))
            return -EFAULT;
        user_info->entry_number = idx;
    }
    if (idx < GDT_ENTRY_TLS_MIN || idx > GDT_ENTRY_TLS_MAX)
        return -EINVAL;

    uint64_t desc = 0;
    if (!user_desc_empty(&info) && !user_desc_zero(&info)) {
        /* Linux's tls_desc_okay(): only present 32-bit data segments in the TLS slots */
        if (!info.seg_32bit || info.contents > 1 || info.seg_not_present)
            return -EINVAL;
        desc = user_desc_encode(&info);
    }
    return pal_to_unix_errno(PalSegmentDescriptorSet(idx, desc));
}

long libos_syscall_get_thread_area(void* u_info) {
    struct user_desc* user_info = u_info;
    if (!is_user_memory_writable(user_info, sizeof(*user_info)))
        return -EFAULT;

    int idx = (int)user_info->entry_number;
    if (idx < GDT_ENTRY_TLS_MIN || idx > GDT_ENTRY_TLS_MAX)
        return -EINVAL;

    uint64_t desc;
    int ret = PalSegmentDescriptorGet(idx, &desc);
    if (ret < 0)
        return pal_to_unix_errno(ret);

    struct user_desc info;
    user_desc_decode(desc, idx, &info);
    memcpy(user_info, &info, sizeof(info));
    return 0;
}
