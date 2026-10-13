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
    for (unsigned int attempt = 0; attempt < 32; attempt++) {
        uint64_t id = __atomic_add_fetch(&next_memfd_id, 1, __ATOMIC_RELAXED);
        int n = snprintf(path, sizeof(path), "/tmp/.gramine-memfd-%u-%llu", g_process.pid,
                         (unsigned long long)id);
        if (n < 0 || (size_t)n >= sizeof(path))
            return -ENAMETOOLONG;
        fd = (int)libos_openat_internal(AT_FDCWD, path, O_RDWR | O_CREAT | O_EXCL |
                                        ((flags & MFD_CLOEXEC) ? O_CLOEXEC : 0), 0600);
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
