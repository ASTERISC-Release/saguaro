/* SPDX-License-Identifier: LGPL-3.0-or-later */
#include "libos_handle.h"
#include "libos_internal.h"
#include "libos_process.h"
#include "libos_table.h"
#include "linux_abi/errors.h"

#define MFD_CLOEXEC       0x0001U
#define MFD_ALLOW_SEALING 0x0002U
#define MFD_HUGETLB       0x0004U
#define MFD_FLAGS         (MFD_CLOEXEC | MFD_ALLOW_SEALING | MFD_HUGETLB)
#define F_SEAL_SEAL       0x0001

long libos_syscall_memfd_create(const char* name, unsigned int flags) {
    if (!name)
        return -EFAULT;
    size_t name_len = 0;
    for (; name_len <= 249; name_len++) {
        if (!is_user_memory_readable(name + name_len, 1))
            return -EFAULT;
        if (!name[name_len])
            break;
    }
    if (name_len > 249)
        return -EINVAL;
    if (flags & ~MFD_FLAGS)
        return -EINVAL;
    if (flags & MFD_HUGETLB)
        return -EPERM;

    static uint64_t next_memfd_id;
    char path[96];
    int fd = -1;
    static const char* const backing_dirs[] = {"/dev/shm", "/tmp"};
    for (unsigned int attempt = 0; attempt < 32; attempt++) {
        uint64_t id = __atomic_add_fetch(&next_memfd_id, 1, __ATOMIC_RELAXED);
        for (size_t i = 0; i < ARRAY_SIZE(backing_dirs); i++) {
            int n = snprintf(path, sizeof(path), "%s/.gramine-memfd-%u-%llu", backing_dirs[i],
                             g_process.pid, (unsigned long long)id);
            if (n < 0 || (size_t)n >= sizeof(path))
                return -ENAMETOOLONG;
            fd = (int)libos_openat_internal(AT_FDCWD, path, O_RDWR | O_CREAT | O_EXCL |
                                            ((flags & MFD_CLOEXEC) ? O_CLOEXEC : 0), 0600);
            if (fd >= 0 || fd == -EEXIST)
                break;
            if (i + 1 == ARRAY_SIZE(backing_dirs) || (fd != -ENOENT && fd != -EROFS))
                return fd;
        }
        if (fd >= 0)
            break;
        if (fd != -EEXIST)
            return fd;
    }
    if (fd < 0)
        return -EEXIST;

    int ret = (int)libos_unlink_internal(path);
    if (ret < 0) {
        libos_syscall_close(fd);
        return ret;
    }

    struct libos_handle* hdl = get_fd_handle(fd, NULL, NULL);
    if (!hdl) {
        libos_syscall_close(fd);
        return -EBADF;
    }
    lock(&hdl->lock);
    hdl->is_memfd = true;
    hdl->memfd_seals = (flags & MFD_ALLOW_SEALING) ? 0 : F_SEAL_SEAL;
    unlock(&hdl->lock);
    put_handle(hdl);
    return fd;
}

int libos_memfd_check_write(struct libos_handle* handle, loff_t offset, size_t count) {
    if (!handle->is_memfd)
        return 0;
    if (handle->memfd_seals & LIBOS_F_SEAL_WRITE)
        return -EPERM;
    /* F_SEAL_FUTURE_WRITE needs no check here: the only way to get a *new* descriptor for a
     * memfd is the /proc/self/fd/N reopen, which refuses write access once the seal is in place
     * (see `libos_open.c`); descriptors that predate the seal keep working. */

    loff_t end;
    if (__builtin_add_overflow(offset, (loff_t)count, &end))
        return -EFBIG;

    if (handle->memfd_seals & LIBOS_F_SEAL_GROW) {
        lock(&handle->inode->lock);
        file_off_t size = handle->inode->size;
        unlock(&handle->inode->lock);
        if (end > size)
            return -EPERM;
    }
    return 0;
}
