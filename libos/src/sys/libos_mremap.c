#include <endian.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "libos_flags_conv.h"
#include "libos_fs.h"
#include "libos_handle.h"
#include "libos_internal.h"
#include "libos_table.h"
#include "libos_vma.h"
#include "linux_abi/errors.h"
#include "linux_abi/memory.h"
#include "pal.h"
#include "pal_error.h"

#ifdef MAP_32BIT /* x86_64-specific */
#define MAP_32BIT_IF_SUPPORTED MAP_32BIT
#else
#define MAP_32BIT_IF_SUPPORTED 0
#endif

#ifndef LEGACY_MAP_MASK
#define LEGACY_MAP_MASK                                                                      \
    (MAP_SHARED | MAP_PRIVATE | MAP_FIXED | MAP_ANONYMOUS | MAP_DENYWRITE | MAP_EXECUTABLE | \
     MAP_UNINITIALIZED | MAP_GROWSDOWN | MAP_LOCKED | MAP_NORESERVE | MAP_POPULATE |         \
     MAP_NONBLOCK | MAP_STACK | MAP_HUGETLB | MAP_32BIT_IF_SUPPORTED | MAP_HUGE_2MB |        \
     MAP_HUGE_1GB)
#endif

static int my_check_prot(int prot) {
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

static void* my_mmap(void* addr, size_t length, int prot, int flags, int fd, unsigned long offset) {
    struct libos_handle* hdl = NULL;
    long ret                 = 0;

    ret = my_check_prot(prot);
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
                    return (void*)-EOPNOTSUPP;
                }
                /* fall through */
            case MAP_PRIVATE:
                if (fd < 0) {
                    return (void*)-EINVAL;
                }

                hdl = get_fd_handle(fd, NULL, NULL);
                if (!hdl) {
                    return (void*)-EBADF;
                }

                if (!hdl->fs || !hdl->fs->fs_ops || !hdl->fs->fs_ops->mmap) {
                    ret = -ENODEV;
                    goto out_handle;
                }

                if (hdl->flags & O_WRONLY) {
                    ret = -EACCES;
                    goto out_handle;
                }

                if ((flags & MAP_SHARED) && (prot & PROT_WRITE) && !(hdl->flags & O_RDWR)) {
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
        memory_range_end   = g_pal_public_state->shared_address_end;
    } else {
        memory_range_start = g_pal_public_state->memory_address_start;
        memory_range_end   = g_pal_public_state->memory_address_end;
    }
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
            ret           = bkeep_munmap(addr, length, /*is_internal=*/false, &tmp_vma);
            if (ret < 0) {
                free_vma_info_array(vmas, vmas_length);
                goto out_handle;
            }

            for (struct libos_vma_info* vma = vmas; vma < vmas + vmas_length; vma++) {
                uintptr_t begin = MAX((uintptr_t)addr, (uintptr_t)vma->addr);
                uintptr_t end   = MIN((uintptr_t)vma->addr + vma->length, (uintptr_t)addr + length);
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
        if (addr && (uintptr_t)memory_range_start <= (uintptr_t)addr &&
            (uintptr_t)addr + length <= (uintptr_t)memory_range_end) {
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
            log_error(
                "[my_mmap] Failed to remove bookkeeped memory that was not allocated at %p-%p!",
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
    return addr;
}

static long my_munmap(void* _addr, size_t length) {
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
        uintptr_t end   = MIN((uintptr_t)vma->addr + vma->length, addr + length);
        /* `vma` contains at least one byte from `[addr; addr + length)` range, so: */
        assert(begin < end);

        void* tmp_vma = NULL;
        ret           = bkeep_munmap((void*)begin, end - begin, /*is_internal=*/false, &tmp_vma);
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

long libos_syscall_mremap(void* old_address, size_t old_size, size_t new_size, int flags,
                          void* new_address) {
    __UNUSED(new_address);

    if (flags != 0 && flags != MREMAP_MAYMOVE) {
        log_error("Only MREMAP_MAYMOVE flag is supported\n");
        return -EINVAL;
    }

    if (!old_address || !IS_ALLOC_ALIGNED_PTR(old_address)) {
        return -EINVAL;
    }

    if (!IS_ALLOC_ALIGNED(old_size))
        old_size = ALLOC_ALIGN_UP(old_size);
    if (!IS_ALLOC_ALIGNED(new_size))
        new_size = ALLOC_ALIGN_UP(new_size);

    if (new_size == 0) {
        return -EINVAL;
    }

    if (old_size == new_size) {
        return (long)old_address;
    }

    if (!access_ok(old_address, old_size)) {
        return -EINVAL;
    }

    // Get VMA attributes at old_address
    struct libos_vma_info vma = {0};
    if (lookup_vma(old_address, &vma) < 0) {
        return -EINVAL;
    }

    // In-place remapping
    if (!(flags & MREMAP_MAYMOVE)) {
        // Save old data
        size_t copy_size = old_size < new_size ? old_size : new_size;
        void* temp_buf   = malloc(copy_size);
        if (!temp_buf) {
            if (vma.file)
                put_handle(vma.file);
            return -ENOMEM;
        }
        memcpy(temp_buf, old_address, copy_size);

        // If shrinking, unmap the tail
        if (new_size < old_size) {
            my_munmap((char*)old_address + new_size, old_size - new_size);
        }

        // Remap in place
        int mmap_flags = vma.flags | MAP_FIXED;
        void* result   = my_mmap(old_address, new_size, vma.prot, mmap_flags, -1, 0);
        if ((long)result < 0) {
            free(temp_buf);
            if (vma.file)
                put_handle(vma.file);
            return (long)result;
        }

        // Restore data
        memcpy(old_address, temp_buf, copy_size);
        free(temp_buf);

        // Zero expanded region if growing
        if (new_size > old_size) {
            memset((char*)old_address + old_size, 0, new_size - old_size);
        }

        if (vma.file)
            put_handle(vma.file);

        return (long)old_address;
    }

    // MREMAP_MAYMOVE
    void* new_addr = my_mmap(NULL, new_size, vma.prot, vma.flags, -1, 0);
    if ((long)new_addr < 0) {
        if (vma.file)
            put_handle(vma.file);
        return (long)new_addr;
    }

    // Copy data
    size_t copy_size = old_size < new_size ? old_size : new_size;
    memcpy(new_addr, old_address, copy_size);

    // Zero expanded region if growing
    if (new_size > old_size) {
        memset((char*)new_addr + old_size, 0, new_size - old_size);
    }

    // Free old region
    long ret = my_munmap(old_address, old_size);
    if (ret < 0) {
        log_error("mremap: failed to unmap old region after allocating new one");
        // Continue anyway, new mapping is valid
    }

    if (vma.file)
        put_handle(vma.file);

    return (long)new_addr;
}
