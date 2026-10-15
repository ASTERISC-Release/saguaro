/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2014 Stony Brook University
 * Copyright (C) 2020 Invisible Things Lab
 * Copyright (C) 2020 Intel Corporation
 *                    Michał Kowalczyk <mkow@invisiblethingslab.com>
 */

/*
 * Implementation of system calls "mmap", "munmap" and "mprotect".
 */

#include "libos_flags_conv.h"
#include "libos_fs.h"
#include "libos_graphics.h"
#include "libos_handle.h"
#include "libos_internal.h"
#include "libos_table.h"
#include "libos_vma.h"
#include "linux_abi/errors.h"
#include "linux_abi/memory.h"
#include "pal.h"
#include "pal_error.h"
#include <endian.h>
#include <stdint.h>
#include <stddef.h>
#ifdef MAP_32BIT /* x86_64-specific */
#define MAP_32BIT_IF_SUPPORTED MAP_32BIT
#else
#define MAP_32BIT_IF_SUPPORTED 0
#endif

#define LEGACY_MAP_MASK (MAP_SHARED             \
                       | MAP_PRIVATE            \
                       | MAP_FIXED              \
                       | MAP_ANONYMOUS          \
                       | MAP_DENYWRITE          \
                       | MAP_EXECUTABLE         \
                       | MAP_UNINITIALIZED      \
                       | MAP_GROWSDOWN          \
                       | MAP_LOCKED             \
                       | MAP_NORESERVE          \
                       | MAP_POPULATE           \
                       | MAP_NONBLOCK           \
                       | MAP_STACK              \
                       | MAP_HUGETLB            \
                       | MAP_32BIT_IF_SUPPORTED \
                       | MAP_HUGE_2MB           \
                       | MAP_HUGE_1GB)

#include <stdatomic.h>
#include <stdbool.h>


static int check_prot(int prot) {
    if (prot & ~(PROT_NONE | PROT_READ | PROT_WRITE | PROT_EXEC | PROT_GROWSDOWN | PROT_GROWSUP |
                 PROT_SEM)) {
        return -EINVAL;
    }

    if ((prot & (PROT_GROWSDOWN | PROT_GROWSUP)) == (PROT_GROWSDOWN | PROT_GROWSUP)) {
        return -EINVAL;
    }

    /* We do not support these flags (at least yet). */
    if (prot & (PROT_GROWSUP | PROT_SEM)) {
        return -EOPNOTSUPP;
    }

    return 0;
}

bool unmapped_data = false;
static void* do_mmap(void* addr, size_t length, int prot, int flags, int fd,
                     unsigned long offset) {

    static bool isMappedComms = false;

    // // // Logging
    // if(isMappedComms)
    //   comm_sync_notify(LOG_MMAP_EVENT, addr, length, prot, flags, fd, offset, 0);

    // First allocation maps comms, then rest of setup hits on next allocation


    struct libos_handle* hdl = NULL;
    long ret = 0;

    ret = check_prot(prot);
    if (ret < 0)
        return (void*)ret;

    if (!(flags & MAP_FIXED) && addr)
        addr = ALLOC_ALIGN_DOWN_PTR(addr);

    /*
     * According to the manpage, both addr and offset have to be page-aligned,
     * but not the length. mmap() will automatically round up the length.
     */
    if (addr && !IS_ALLOC_ALIGNED_PTR(addr))
        return (void*)-EINVAL;

    if (fd >= 0 && !IS_ALLOC_ALIGNED(offset))
        return (void*)-EINVAL;

    if (!IS_ALLOC_ALIGNED(length))
        length = ALLOC_ALIGN_UP(length);

    if (!length || !access_ok(addr, length))
        return (void*)-EINVAL;

    /* This check is Gramine specific. */
    if (flags & (VMA_UNMAPPED | VMA_TAINTED | VMA_INTERNAL)) {
        return (void*)-EINVAL;
    }

    if (flags & MAP_ANONYMOUS) {
        switch (flags & MAP_TYPE) {
            case MAP_SHARED:
            case MAP_PRIVATE:
                break;
            default:
                return (void*)-EINVAL;
        }
    } else {
        /* MAP_FILE is the opposite of MAP_ANONYMOUS and is implicit */
        switch (flags & MAP_TYPE) {
            case MAP_SHARED:
                flags &= LEGACY_MAP_MASK;
                /* fall through */
            case MAP_SHARED_VALIDATE:
                /* Currently we do not support additional flags like MAP_SYNC */
                if (flags & ~LEGACY_MAP_MASK) {
                    log_always("error: mmap with unsupported flags (flags: 0x%x)", flags);
                    return (void*)-EOPNOTSUPP;
                }
                /* fall through */
            case MAP_PRIVATE:
                if (fd < 0) {
                    return (void*)-EINVAL;
                }

                hdl = get_fd_handle(fd, NULL, NULL);
                if (!hdl) {
                    log_always("error: mmap with invalid fd (fd: %d)", fd);
                    return (void*)-EBADF;
                }
                if (hdl->is_memfd) {
                    /* F_SEAL_EXEC refuses executable mappings, shared or private. */
                    if ((prot & PROT_EXEC) && (hdl->memfd_seals & LIBOS_F_SEAL_EXEC)) {
                        ret = -EPERM;
                        goto out_handle;
                    }
                    if (flags & MAP_SHARED) {
                        /* F_SEAL_WRITE refuses new writable shared mappings;
                         * F_SEAL_FUTURE_WRITE refuses them from the moment the seal is added (the
                         * mappings that predate the seal stay usable, checked in mprotect). */
                        if ((prot & PROT_WRITE) && (hdl->memfd_seals & (LIBOS_F_SEAL_WRITE
                                                                        | LIBOS_F_SEAL_FUTURE_WRITE))) {
                            ret = -EPERM;
                            goto out_handle;
                        }
                    }
                }

                // log_always("mmap request (fd: %d, offset: %lu, length: %zu, prot: 0x%x, flags: 0x%x)",
                //            fd, offset, length, prot, flags);

                // Graphics (check the file descriptor type and have the host allocate)
                #ifdef ENABLE_GRAPHICS
                    if(hdl->type == TYPE_HOST)
                    {
                        long _offset = libos_syscall_acquire_libos_lock();
            comm_page_t* c = comm_page(_offset);

                        // Notify that this object should be mapped from host to guest
                        /* The listener chooses the next identity-mapped arena address, so the
                         * requested guest address is not part of the host ABI. */
                        c->p1 = (uint64_t)length;
                        c->p2 = (uint64_t)prot;
                        c->p3 = (uint64_t)flags;
                        c->p4 = (uint64_t)fd;
                        c->p5 = (uint64_t)offset;

                        __sync_synchronize();
                        c->req_bit = GEM_ALLOCATION;

                        void* retval = (void*)comm_sync_notify(c);
                        libos_syscall_relinquish_libos_lock(_offset);
                        if ((int64_t)(uintptr_t)retval < 0 && (int64_t)(uintptr_t)retval >= -4095) {
                            /* the listener could not map it (arenas full: -ENOMEM) */
                            put_handle(hdl);
                            return retval;
                        }

                        /* QEMU replaced this identity-mapped arena slice with
                         * the GEM object. Apply the requested permissions and
                         * mark only this live slice write-combining in the VM
                         * PAL, rather than making the entire multi-GiB arena WC. */
                        ret = PalVirtualMemoryProtect(retval, length,
                                                      LINUX_PROT_TO_PAL(prot, flags));
                        if (ret < 0)
                            retval = (void*)pal_to_unix_errno(ret);

                        put_handle(hdl);
                        return retval;
                   }
                #endif

                if (!hdl->fs || !hdl->fs->fs_ops || !hdl->fs->fs_ops->mmap) {
                    log_always("error: mmap of fd without mmap support (fd: %d)", fd);
                    ret = -ENODEV;
                    goto out_handle;
                }

                if (hdl->flags & O_WRONLY) {
                    log_always("error: mmap of write-only fd is not allowed (fd: %d)", fd);
                    ret = -EACCES;
                    goto out_handle;
                }

                if ((flags & MAP_SHARED) && (prot & PROT_WRITE) && !(hdl->flags & O_RDWR)) {
                    log_always("error: mmap with MAP_SHARED and PROT_WRITE requires fd with O_RDWR (fd: %d)", fd);
                    ret = -EACCES;
                    goto out_handle;
                }

                break;
            default:
                return (void*)-EINVAL;
        }
    }

#ifdef MAP_32BIT
    /* ignore MAP_32BIT when MAP_FIXED is set */
    if ((flags & (MAP_32BIT | MAP_FIXED)) == (MAP_32BIT | MAP_FIXED))
        flags &= ~MAP_32BIT;
#endif

    void* memory_range_start = NULL;
    void* memory_range_end   = NULL;

    /* Shared mappings of files of "untrusted_shm" type use a different memory range.
     * See "libos/src/fs/shm/fs.c" for more details. */
    if ((flags & MAP_SHARED) && hdl && hdl->fs && !strcmp(hdl->fs->name, "untrusted_shm")) {
        memory_range_start = g_pal_public_state->shared_address_start;
        memory_range_end = g_pal_public_state->shared_address_end;
    } else {
        memory_range_start = g_pal_public_state->memory_address_start;
        memory_range_end = g_pal_public_state->memory_address_end;
    }

#ifdef PAL_HOST_VBS
    extern void libos_syscall_entry(void); /* libos_entry.h */
    /* VBS-enclave PAL (-Dvbs=enabled builds only): code cannot be mapped at run time; a packed
     * shared object (packed into the signed enclave image at build time) is used where it lies. A
     * dynamic loader's first, executable mapping of such a file (offset 0, no fixed address) is
     * therefore placed at the object's packed address, and the object's syscall-entry slot is
     * filled; its later MAP_FIXED mappings then match the packed segments. */
    if (hdl && VBS_FIRST_MAP_PROT_OK(prot) && !(flags & (MAP_FIXED | MAP_FIXED_NOREPLACE)) && offset == 0
            && g_pal_public_state->vbs_packed_lookup) {
        uintptr_t packed_base, packed_slot;
        size_t packed_size;
        if (g_pal_public_state->vbs_packed_lookup(hdl->uri, &packed_base, &packed_size,
                                                  &packed_slot) == 0 && length <= packed_size VBS_FIRST_MAP_LEN_OK(prot, length, packed_size)) {
            if (packed_slot)
                *(uintptr_t*)packed_slot = (uintptr_t)&libos_syscall_entry;
            addr = (void*)packed_base;
            flags |= MAP_FIXED;
        }
    }
#endif
    if (flags & (MAP_FIXED | MAP_FIXED_NOREPLACE)) {
        /* We know that `addr + length` does not overflow (`access_ok` above). */
        if (addr < memory_range_start || (uintptr_t)memory_range_end < (uintptr_t)addr + length) {
            ret = -EINVAL;
            goto out_handle;
        }
        if (!(flags & MAP_FIXED_NOREPLACE)) {
            /* Flush any file mappings we're about to replace */
            ret = msync_range((uintptr_t)addr, (uintptr_t)addr + length);
            if (ret < 0) {
                goto out_handle;
            }

            struct libos_vma_info* vmas;
            size_t vmas_length;
            ret = dump_vmas_in_range((uintptr_t)addr, (uintptr_t)addr + length,
                                     /*include_unmapped=*/false, &vmas, &vmas_length);
            if (ret < 0) {
                goto out_handle;
            }

            void* tmp_vma = NULL;
            ret = bkeep_munmap(addr, length, /*is_internal=*/false, &tmp_vma);
            if (ret < 0) {
                free_vma_info_array(vmas, vmas_length);
                goto out_handle;
            }

            for (struct libos_vma_info* vma = vmas; vma < vmas + vmas_length; vma++) {
                uintptr_t begin = MAX((uintptr_t)addr, (uintptr_t)vma->addr);
                uintptr_t end = MIN((uintptr_t)vma->addr + vma->length, (uintptr_t)addr + length);
                /* `vma` contains at least one byte from `[addr; addr + length)` range, so: */
                assert(begin < end);

                if (PalVirtualMemoryFree((void*)begin, end - begin) < 0) {
                    BUG();
                }
            }

            free_vma_info_array(vmas, vmas_length);

            bkeep_convert_tmp_vma_to_user(tmp_vma);

            ret = bkeep_mmap_fixed(addr, length, prot, flags, hdl, offset, NULL);
            if (ret < 0) {
                BUG();
            }
        } else {
            ret = bkeep_mmap_fixed(addr, length, prot, flags, hdl, offset, NULL);
            if (ret < 0) {
                goto out_handle;
            }
        }
    } else {
        /* We know that `addr + length` does not overflow (`access_ok` above). */
        if (addr && (uintptr_t)memory_range_start <= (uintptr_t)addr
                && (uintptr_t)addr + length <= (uintptr_t)memory_range_end) {
            ret = bkeep_mmap_any_in_range(memory_range_start, (char*)addr + length, length, prot,
                                          flags, hdl, offset, NULL, &addr);
        } else {
            /* Hacky way to mark we had no hit and need to search below. */
            ret = -1;
        }
        if (ret < 0) {
            /* We either had no hinted address or could not allocate memory at it. */
            if (memory_range_start == g_pal_public_state->memory_address_start) {
                ret = bkeep_mmap_any_aslr(length, prot, flags, hdl, offset, NULL, &addr);
            } else {
                /* Shared memory range does not have ASLR. */
                ret = bkeep_mmap_any_in_range(memory_range_start, memory_range_end, length, prot,
                                              flags, hdl, offset, NULL, &addr);
            }
        }
        if (ret < 0) {
            ret = -ENOMEM;
            goto out_handle;
        }
    }

    /* From now on `addr` contains the actual address we want to map (and already bookkeeped). */

    if (!hdl) {
        ret = PalVirtualMemoryAlloc(addr, length, LINUX_PROT_TO_PAL(prot, flags));
        if (ret < 0) {
            if (ret == -PAL_ERROR_DENIED) {
                ret = -EPERM;
            } else {
                ret = pal_to_unix_errno(ret);
            }
        }
    } else {
        ret = hdl->fs->fs_ops->mmap(hdl, addr, length, prot, flags, offset);
    }

    if (ret < 0) {
        void* tmp_vma = NULL;
        if (bkeep_munmap(addr, length, /*is_internal=*/false, &tmp_vma) < 0) {
            log_error("[mmap] Failed to remove bookkeeped memory that was not allocated at %p-%p!",
                      addr, (char*)addr + length);
            BUG();
        }
        bkeep_remove_tmp_vma(tmp_vma);
    }

out_handle:
    if (hdl) {
        put_handle(hdl);
    }

    if (ret < 0) {
        return (void*)ret;
    }

#ifdef ENABLE_GRAPHICS
    /* The application (its graphics preload library) maps the comm window itself before the
     * handshake with the host listener; only from then on may the LibOS write to it. */
    if ((flags & MAP_FIXED) && (uintptr_t)addr <= COMM_ADDR
            && COMM_ADDR < (uintptr_t)addr + length) {
        __atomic_store_n(&g_graphics_comm_mapped, true, __ATOMIC_RELEASE);
    }
#endif

    return addr;
}

static long do_mprotect(void* addr, size_t length, int prot) {
    int ret = check_prot(prot);
    if (ret < 0)
        return ret;

    /*
     * According to the manpage, addr has to be page-aligned, but not the
     * length. mprotect() will automatically round up the length.
     */
    if (!IS_ALLOC_ALIGNED_PTR(addr))
        return -EINVAL;

    if (length == 0) {
        return 0;
    }

    if (!IS_ALLOC_ALIGNED(length))
        length = ALLOC_ALIGN_UP(length);

    /* a range that is not entirely mapped is ENOMEM, as in the kernel (LTP mprotect01) */
    if (!addr || !access_ok(addr, length) || !is_in_adjacent_user_vmas(addr, length, /*prot=*/0)) {
        return -ENOMEM;
    }

    if (prot & (PROT_WRITE | PROT_EXEC)) {
        struct libos_vma_info vma_info = {0};
        if (lookup_vma(addr, &vma_info) >= 0) {
            bool memfd_vma = vma_info.file && vma_info.file->is_memfd;
            unsigned int seals = memfd_vma ? vma_info.file->memfd_seals : 0;
            /* a shared file mapping of a descriptor opened without write access cannot be made
             * writable (EACCES) */
            bool no_write = vma_info.file && (vma_info.flags & MAP_SHARED) &&
                            !(vma_info.file->acc_mode & MAY_WRITE) && !memfd_vma;
            if (vma_info.file)
                put_handle(vma_info.file);
            if (no_write && (prot & PROT_WRITE) && !(vma_info.prot & PROT_WRITE))
                return -EACCES;
            if (memfd_vma && (prot & PROT_EXEC) && (seals & LIBOS_F_SEAL_EXEC))
                return -EPERM;
            /* making a shared mapping writable counts as new writable access unless it was
             * already writable; `F_SEAL_WRITE` refuses that always, `F_SEAL_FUTURE_WRITE` only
             * once the seal is in place (mappings already writable when the seal was added keep
             * working). Private mappings never write through to the file. */
            if (memfd_vma && (vma_info.flags & MAP_SHARED) && (prot & PROT_WRITE)
                    && !(vma_info.prot & PROT_WRITE)
                    && (seals & (LIBOS_F_SEAL_WRITE | LIBOS_F_SEAL_FUTURE_WRITE)))
                return -EPERM;
        }
    }

    /* `bkeep_mprotect` and then `PalVirtualMemoryProtect` is racy, but it's hard to do it properly.
     * On the other hand if this race happens, it means user app is buggy, so not a huge problem. */

    ret = bkeep_mprotect(addr, length, prot, /*is_internal=*/false);
    if (ret < 0) {
        return ret;
    }

    if (prot & PROT_GROWSDOWN) {
        struct libos_vma_info vma_info = {0};
        if (lookup_vma(addr, &vma_info) >= 0) {
            assert(vma_info.addr <= addr);
            length += addr - vma_info.addr;
            addr = vma_info.addr;
            if (vma_info.file) {
                put_handle(vma_info.file);
            }
        } else {
            log_warning("Memory that was about to be mprotected was unmapped, your program is "
                        "buggy!");
            return -ENOTRECOVERABLE;
        }
    }

    ret = PalVirtualMemoryProtect(addr, length, LINUX_PROT_TO_PAL(prot, /*map_flags=*/0));
    if (ret < 0) {
        return pal_to_unix_errno(ret);
    }

    return 0;
}

static long do_munmap(void* _addr, size_t length) {
    uintptr_t addr = (uintptr_t)_addr;
    /*
     * According to the manpage, addr has to be page-aligned, but not the
     * length. munmap() will automatically round up the length.
     */
    if (!addr || !IS_ALLOC_ALIGNED(addr))
        return -EINVAL;

    if (!length || !access_ok(_addr, length))
        return -EINVAL;

    if (!IS_ALLOC_ALIGNED(length))
        length = ALLOC_ALIGN_UP(length);

#ifdef ENABLE_GRAPHICS
    /* Host-device mappings replace pages inside libgraphics's permanently reserved GEM arenas,
     * but the host mmap fast path above deliberately leaves the original LibOS VMA reservation
     * intact. Releasing a subrange here would make it available to the guest allocator while
     * QEMU and the GPU still use the same identity-mapped addresses, allowing GPU writes to
     * corrupt an unrelated heap allocation. The host allocator is monotonic for the VM lifetime,
     * so preserve these reservations until process teardown.
     *
     * The data-arena size is runtime-configurable (manifest key
     * sys.experimental__graphics_data_size) and the hugepage arena follows the data arena; the
     * PAL reserves exactly these ranges (kernel_memory.c). Read the bounds once; a concurrent
     * second initialisation computes identical values. */
    static bool gem_bounds_ready;
    static uint64_t gem_data_start, gem_data_end, gem_huge_start, gem_huge_end;
    if (!__atomic_load_n(&gem_bounds_ready, __ATOMIC_ACQUIRE)) {
        /* The arena bounds are exactly the ranges the PAL reserved for them
         * (comments "egl_redirect_data" and "egl_redirect_huge", see
         * kernel_memory.c); a guest without a graphics deployment has none, and
         * the guards below become no-ops. A concurrent second initialisation
         * computes identical values. */
        for (size_t i = 0; i < g_pal_public_state->initial_mem_ranges_len; i++) {
            struct pal_initial_mem_range* r = &g_pal_public_state->initial_mem_ranges[i];
            if (r->is_free) {
                continue;
            }
            if (!strcmp(r->comment, "egl_redirect_data")) {
                gem_data_start = r->start;
                gem_data_end   = r->end;
            } else if (!strcmp(r->comment, "egl_redirect_huge")) {
                gem_huge_start = r->start;
                gem_huge_end   = r->end;
            }
        }
        __atomic_store_n(&gem_bounds_ready, true, __ATOMIC_RELEASE);
    }
    bool in_regular_gem_arena =
        addr >= gem_data_start && length <= gem_data_end - gem_data_start &&
        addr - gem_data_start <= (gem_data_end - gem_data_start) - length;
    bool in_hugepage_gem_arena =
        addr >= gem_huge_start && length <= gem_huge_end - gem_huge_start &&
        addr - gem_huge_start <= (gem_huge_end - gem_huge_start) - length;
    if (in_regular_gem_arena || in_hugepage_gem_arena) {
        /* The reservation itself stays (see above), but the host listener can reuse the range:
         * it drops its device mapping there and puts the range on its free list. GEM_FREE is an
         * ABI addition: the listener has to be built from the same tree (graphics_comm_abi.h). */
        if (graphics_redirect_enabled()) {
            long _offset = libos_syscall_acquire_libos_lock();
            comm_page_t* c = comm_page(_offset);
            c->p1 = (uint64_t)addr;
            c->p2 = (uint64_t)length;
            c->req_bit = GEM_FREE;
            (void)comm_sync_notify(c);
            libos_syscall_relinquish_libos_lock(_offset);
        }
        return 0;
    }
#endif

    int ret;

    /* Flush any file mappings we're about to remove */
    ret = msync_range(addr, addr + length);
    if (ret < 0) {
        return ret;
    }

    struct libos_vma_info* vmas;
    size_t vmas_length;
    ret = dump_vmas_in_range(addr, addr + length, /*include_unmapped=*/false, &vmas, &vmas_length);
    if (ret < 0) {
        return ret;
    }

    for (struct libos_vma_info* vma = vmas; vma < vmas + vmas_length; vma++) {
        uintptr_t begin = MAX(addr, (uintptr_t)vma->addr);
        uintptr_t end = MIN((uintptr_t)vma->addr + vma->length, addr + length);
        /* `vma` contains at least one byte from `[addr; addr + length)` range, so: */
        assert(begin < end);

        void* tmp_vma = NULL;
        ret = bkeep_munmap((void*)begin, end - begin, /*is_internal=*/false, &tmp_vma);
        if (ret < 0) {
            BUG();
        }

        if (PalVirtualMemoryFree((void*)begin, end - begin) < 0) {
            BUG();
        }

        bkeep_remove_tmp_vma(tmp_vma);
    }

    free_vma_info_array(vmas, vmas_length);
    return 0;
}

/* This emulation of mincore() always pessimistically tells that pages are _NOT_ in RAM due to lack
 * of a good way to know it.
 * This lying may possibly cause performance (or other) issues.
 */
long libos_syscall_mincore(void* addr, size_t len, unsigned char* vec) {
    if (!IS_ALLOC_ALIGNED_PTR(addr))
        return -EINVAL;

    if (!access_ok(addr, len)) {
        return -ENOMEM;
    }

    if (!is_in_adjacent_user_vmas(addr, len, /*prot=*/0)) {
        return -ENOMEM;
    }

    unsigned long pages = ALLOC_ALIGN_UP(len) / ALLOC_ALIGNMENT;
    if (!is_user_memory_writable(vec, pages))
        return -EFAULT;

    if (FIRST_TIME()) {
        log_warning("mincore emulation always tells pages are _NOT_ in RAM. This may cause "
                    "issues.");
    }

    /* There is no good way to know if the page is in RAM.
     * Conservatively tell that it's not in RAM. */
    for (unsigned long i = 0; i < pages; i++) {
        vec[i] = 0;
    }

    return 0;
}

long libos_syscall_mbind(void* start, unsigned long len, int mode, unsigned long* nmask,
                         unsigned long maxnode, int flags) {
    /* dummy implementation, always return success */
    __UNUSED(start);
    __UNUSED(len);
    __UNUSED(mode);
    __UNUSED(nmask);
    __UNUSED(maxnode);
    __UNUSED(flags);
    return 0;
}

static bool madvise_behavior_valid(int behavior) {
    switch (behavior) {
        case MADV_DOFORK:
        case MADV_DONTFORK:
        case MADV_NORMAL:
        case MADV_SEQUENTIAL:
        case MADV_RANDOM:
        case MADV_REMOVE:
        case MADV_WILLNEED:
        case MADV_DONTNEED:
        case MADV_FREE:
        case MADV_MERGEABLE:
        case MADV_UNMERGEABLE:
        case MADV_HUGEPAGE:
        case MADV_NOHUGEPAGE:
        case MADV_DONTDUMP:
        case MADV_DODUMP:
        case MADV_WIPEONFORK:
        case MADV_KEEPONFORK:
        case MADV_SOFT_OFFLINE:
        case MADV_HWPOISON:
            return true;
    }
    return false;
}

long libos_syscall_madvise(unsigned long start, size_t len_in, int behavior) {
    if (!madvise_behavior_valid(behavior))
        return -EINVAL;

    if (!IS_ALIGNED_POW2(start, PAGE_SIZE))
        return -EINVAL;

    size_t len = ALIGN_UP(len_in, PAGE_SIZE);
    if (len < len_in)
        return -EINVAL; // overflow when rounding up

    if (!access_ok((void*)start, len))
        return -EINVAL;

    if (len == 0)
        return 0;

    switch (behavior) {
        case MADV_NORMAL:
        case MADV_RANDOM:
        case MADV_SEQUENTIAL:
        case MADV_WILLNEED:
        case MADV_FREE:
        case MADV_SOFT_OFFLINE:
        case MADV_MERGEABLE:
        case MADV_UNMERGEABLE:
        case MADV_HUGEPAGE:
        case MADV_NOHUGEPAGE:
            return 0; // Doing nothing is semantically correct for these modes.

        case MADV_DONTFORK:
        case MADV_DOFORK:
        case MADV_WIPEONFORK:
        case MADV_KEEPONFORK:
        case MADV_HWPOISON:
        case MADV_DONTDUMP:
        case MADV_DODUMP:
        case MADV_REMOVE:
            return -ENOSYS; // Not implemented

        case MADV_DONTNEED: {
            return madvise_dontneed_range(start, start + len);
        }
    }
    return -EINVAL;
}

long libos_syscall_msync(unsigned long start, size_t len_orig, int flags) {
    if (flags & ~(MS_ASYNC | MS_SYNC | MS_INVALIDATE)) {
        return -EINVAL;
    }

    if ((flags & MS_ASYNC) && (flags & MS_SYNC)) {
        return -EINVAL;
    }

    if (!IS_ALIGNED_POW2(start, PAGE_SIZE)) {
        return -EINVAL;
    }

    size_t len = ALIGN_UP_POW2(len_orig, PAGE_SIZE);
    if (len < len_orig) {
        return -ENOMEM;
    }

    if (!(flags & (MS_SYNC | MS_ASYNC))) {
        /* Currently Linux permits a call without either `MS_SYNC` or `MS_ASYNC`, and treats it as
         * equivalent to specifying `MS_ASYNC`. */
        flags |= MS_ASYNC;
    }

    /* ENOMEM only for a range that is not mapped (the kernel's answer): a readability probe would
     * also refuse mapped pages that fault, e.g. a file mapping past the end of its file, or
     * PROT_NONE ones (LTP mremap01, qmm01) */
    if (len && !is_in_adjacent_user_vmas((void*)start, len, /*prot=*/0)) {
        return -ENOMEM;
    }

    if (flags & MS_INVALIDATE) {
        log_warning("Gramine does not support MS_INVALIDATE");
        return -ENOSYS;
    }

    if (flags & MS_SYNC) {
        return msync_range(start, start + len);
    } else {
        /* `MS_ASYNC` is a no-op on Linux. */
        return 0;
    }
}

/* The address-space-changing calls run under g_vma_change_lock (shared), so that fork does not
 * copy memory that is being unmapped or remapped (see libos_vma.c). */
void* libos_syscall_mmap(void* addr, size_t length, int prot, int flags, int fd,
                         unsigned long offset) {
    rwlock_read_lock(&g_vma_change_lock);
    void* ret = do_mmap(addr, length, prot, flags, fd, offset);
    rwlock_read_unlock(&g_vma_change_lock);
    return ret;
}

long libos_syscall_mprotect(void* addr, size_t length, int prot) {
    rwlock_read_lock(&g_vma_change_lock);
    long ret = do_mprotect(addr, length, prot);
    rwlock_read_unlock(&g_vma_change_lock);
    return ret;
}

long libos_syscall_munmap(void* addr, size_t length) {
    rwlock_read_lock(&g_vma_change_lock);
    long ret = do_munmap(addr, length);
    rwlock_read_unlock(&g_vma_change_lock);
    return ret;
}
