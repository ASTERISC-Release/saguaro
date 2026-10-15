/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/*
 * mremap(): shrink a mapping in place, grow it in place when the pages behind it are free, or
 * move it (MREMAP_MAYMOVE, to any address or to MREMAP_FIXED's), keeping the old range with
 * MREMAP_DONTUNMAP. Built on the LibOS's own mmap/munmap/mprotect, so the VMA bookkeeping stays
 * consistent. A moved file mapping is mapped again from the file at the matching offset (a
 * private one also gets its modified contents copied); a moved anonymous mapping is copied. The
 * one difference from Linux: a moved MAP_SHARED|MAP_ANONYMOUS mapping becomes a copy, no longer
 * shared with a forked child (the PAL cannot map the same anonymous pages twice). The checks
 * follow the kernel's order (LTP mremap01-05). Device mappings (render nodes, host fds) can only
 * shrink.
 */

#include "libos_flags_conv.h"
#include "libos_fs.h"
#include "libos_handle.h"
#include "libos_internal.h"
#include "libos_table.h"
#include "libos_vma.h"
#include "stat.h"
#include "linux_abi/errors.h"
#include "linux_abi/memory.h"

#ifndef MREMAP_MAYMOVE
#define MREMAP_MAYMOVE 1
#endif
#ifndef MREMAP_FIXED
#define MREMAP_FIXED 2
#endif
#ifndef MREMAP_DONTUNMAP
#define MREMAP_DONTUNMAP 4
#endif

/* the MAP_* flags a mapping is recreated with */
#define REMAP_KEEP_FLAGS (MAP_SHARED | MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_GROWSDOWN)

static bool is_error(void* ptr) {
    return (long)ptr < 0 && (long)ptr >= -4095;
}

/* Map `len` bytes at `addr` like the VMA `vma`, continuing (or replacing) its part at byte
 * `src_offset` from its start. */
static void* map_like(const struct libos_vma_info* vma, size_t src_offset, void* addr, size_t len,
                      int prot, int extra_flags) {
    int flags = (vma->flags & REMAP_KEEP_FLAGS) | extra_flags;
    if (!vma->file || (vma->flags & MAP_ANONYMOUS))
        return (void*)libos_syscall_mmap(addr, len, prot, flags | MAP_ANONYMOUS, -1, 0);

    /* the file is mapped through a temporary descriptor of the VMA's own handle */
    int fd = set_new_fd_handle(vma->file, /*fd_flags=*/0, NULL);
    if (fd < 0)
        return (void*)(long)fd;
    void* ret = (void*)libos_syscall_mmap(addr, len, prot, flags, fd,
                                          vma->file_offset + src_offset);
    struct libos_handle* tmp = detach_fd_handle(fd, NULL, NULL);
    if (tmp)
        put_handle(tmp);
    return ret;
}

/*
 * Linux requires the old range to lie in one VMA (`vma_to_resize()`: EFAULT otherwise). The LibOS
 * does not merge adjacent VMAs, so a range may span several; it is accepted only if they are what
 * Linux would have merged: contiguous user VMAs with the same protection, flags and file, the file
 * offsets continuing. A range reaching into a different mapping (e.g. LTP's one-page PROT_NONE
 * "bad address" with libc above it) is EFAULT; treating it as one mapping changed the protection
 * of the neighbours and crashed the process.
 */
static bool is_one_mapping(uintptr_t start, size_t size, const struct libos_vma_info* first) {
    uintptr_t end = start + size;
    uintptr_t cur = (uintptr_t)first->addr + first->length;
    while (cur < end) {
        struct libos_vma_info next = {0};
        if (lookup_vma((void*)cur, &next) < 0)
            return false;
        bool same = (uintptr_t)next.addr == cur && next.prot == first->prot &&
                    next.flags == first->flags && next.file == first->file &&
                    (!first->file || next.file_offset == first->file_offset +
                                                         (cur - (uintptr_t)first->addr));
        cur = (uintptr_t)next.addr + next.length;
        if (next.file)
            put_handle(next.file);
        if (!same)
            return false;
    }
    return true;
}

long libos_syscall_mremap(void* old_address, size_t old_size, size_t new_size, int flags,
                          void* new_address) {
    if (flags & ~(MREMAP_MAYMOVE | MREMAP_FIXED | MREMAP_DONTUNMAP))
        return -EINVAL;
    if ((flags & MREMAP_FIXED) && !(flags & MREMAP_MAYMOVE))
        return -EINVAL;
    if ((flags & MREMAP_DONTUNMAP) && (!(flags & MREMAP_MAYMOVE) || old_size != new_size))
        return -EINVAL;
    if (!IS_ALLOC_ALIGNED_PTR(old_address))
        return -EINVAL;

    old_size = ALLOC_ALIGN_UP(old_size);
    new_size = ALLOC_ALIGN_UP(new_size);
    if (!new_size)
        return -EINVAL;
    /* old_size 0 duplicates a shared mapping on Linux; the PAL cannot map pages twice */
    if (!old_size)
        return -EINVAL;

    uintptr_t old = (uintptr_t)old_address;
    uintptr_t target = 0;
    if (flags & MREMAP_FIXED) {
        target = (uintptr_t)new_address;
        if (!IS_ALLOC_ALIGNED(target))
            return -EINVAL;
        /* the new range must not overlap the old one */
        if (target < old + old_size && old < target + new_size)
            return -EINVAL;
    }
    if (!access_ok(old_address, old_size) ||
            ((flags & MREMAP_FIXED) && !access_ok((void*)target, new_size)))
        return -EINVAL;

    /* the old range must be mapped without holes and be one mapping (`is_one_mapping()`); the
     * first VMA gives the attributes of a move */
    if (!is_in_adjacent_user_vmas(old_address, old_size, /*prot=*/0))
        return -EFAULT;
    struct libos_vma_info vma = {0};
    if (lookup_vma(old_address, &vma) < 0)
        return -EFAULT;
    long ret;
    size_t src_offset = old - (uintptr_t)vma.addr;
    if (!is_one_mapping(old, old_size, &vma)) {
        ret = -EFAULT;
        goto out;
    }

    /* A device mapping (for example a GPU buffer mapped through a render node): the copy below
     * would detach the mapping from the device. Shrinking only drops the tail; growing or moving
     * is not supported. */
    if (vma.file && ((vma.file->type == TYPE_CHROOT && vma.file->inode &&
                      vma.file->inode->type == S_IFCHR) || vma.file->type == TYPE_HOSTFD)) {
        if (new_size < old_size) {
            ret = libos_syscall_munmap((void*)(old + new_size), old_size - new_size);
            if (ret == 0)
                ret = (long)old;
        } else {
            log_warning("mremap: growing or moving a device mapping is not supported");
            ret = -ENOMEM;
        }
        goto out;
    }

    /* A shared file mapping is written back first: filesystems with emulated mappings (tmpfs)
     * hand a new mapping (an extension or the moved range) the file's contents, not the old
     * mapping's pages. */
    if (vma.file && (vma.flags & MAP_SHARED))
        (void)libos_syscall_msync((unsigned long)old_address, old_size, MS_SYNC);

    if (!(flags & (MREMAP_FIXED | MREMAP_DONTUNMAP))) {
        if (new_size <= old_size) {
            /* shrinking (or the same size) stays in place */
            if (new_size < old_size) {
                ret = libos_syscall_munmap((void*)(old + new_size), old_size - new_size);
                if (ret < 0)
                    goto out;
            }
            ret = (long)old;
            goto out;
        }

        /* growing in place, if the pages behind the mapping are free (the extension continues
         * the VMA holding the last page; a range ending inside a VMA has a neighbour there) */
        struct libos_vma_info last = {0};
        if (lookup_vma((void*)(old + old_size - 1), &last) == 0) {
            uintptr_t last_end = (uintptr_t)last.addr + last.length;
            if (old + old_size == last_end) {
                void* ext = map_like(&last, last.length, (void*)last_end, new_size - old_size,
                                     last.prot, MAP_FIXED_NOREPLACE);
                if (last.file)
                    put_handle(last.file);
                if (!is_error(ext) && ext != (void*)last_end) {
                    /* the LibOS placed it elsewhere (the pages behind are taken) */
                    (void)libos_syscall_munmap(ext, new_size - old_size);
                    ext = (void*)-EEXIST;
                }
                if (!is_error(ext)) {
                    ret = (long)old;
                    goto out;
                }
            } else if (last.file) {
                put_handle(last.file);
            }
        }
        if (!(flags & MREMAP_MAYMOVE)) {
            ret = -ENOMEM;
            goto out;
        }
    }

    /* move: map the new range (writable for the copy), copy, then drop the old range */
    int map_prot = vma.prot | PROT_WRITE;
    void* moved = map_like(&vma, src_offset, (void*)target, new_size, map_prot,
                           (flags & MREMAP_FIXED) ? MAP_FIXED : 0);
    if (is_error(moved)) {
        ret = (long)moved;
        goto out;
    }

    bool copy = !vma.file || (vma.flags & (MAP_ANONYMOUS | MAP_PRIVATE));
    if (copy) {
        size_t copy_size = MIN(old_size, new_size);
        bool readable = vma.prot & PROT_READ;
        if (!readable)
            (void)libos_syscall_mprotect(old_address, old_size, vma.prot | PROT_READ);
        memcpy(moved, old_address, copy_size);
        if (!readable)
            (void)libos_syscall_mprotect(old_address, old_size, vma.prot);
    }
    if (map_prot != vma.prot)
        (void)libos_syscall_mprotect(moved, new_size, vma.prot);

    if (flags & MREMAP_DONTUNMAP) {
        /* the old range stays mapped, emptied (anonymous mappings read as zeros again) */
        if (vma.flags & MAP_ANONYMOUS)
            (void)map_like(&vma, src_offset, old_address, old_size, vma.prot, MAP_FIXED);
    } else {
        ret = libos_syscall_munmap(old_address, old_size);
        if (ret < 0)
            log_warning("mremap: unmapping the old range failed: %ld", ret);
    }
    ret = (long)moved;

out:
    if (vma.file)
        put_handle(vma.file);
    return ret;
}
