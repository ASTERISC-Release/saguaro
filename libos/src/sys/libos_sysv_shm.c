/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/*
 * System V shared memory (shmget/shmat/shmctl/shmdt).
 *
 * Each segment is backed by a host file (the same /dev/shm-then-/tmp backing as `memfd_create`,
 * see `libos_memfd.c`), unlinked at creation so a killed Gramine process leaves no file behind.
 * `shmat()` maps that file `MAP_SHARED` through the ordinary `mmap` path, so two attaches of the
 * same segment share pages inside one process, and — on the direct PAL, whose `fork` is a host
 * fork — with the process's children too, since both map the same host file. Segment bookkeeping
 * (the id table) is process-local: a separate Gramine process starts with an empty table, so the
 * key namespace and the permissions of `shmctl` are only enforced within one process tree. This
 * matches how the System V semaphores are emulated (`libos_sem.c`) and is recorded in
 * dev docs: libos-syscalls/TODO.md.
 */

#include "libos_handle.h"
#include "libos_internal.h"
#include "libos_sysv_shared.h"
#include "libos_lock.h"
#include "libos_process.h"
#include "libos_table.h"
#include "libos_vma.h"
#include "linux_abi/errors.h"
#include "linux_abi/ipc.h"
#include "linux_abi/memory.h"
#include "pal.h"

#define MAX_SYSV_SHM_SEGS   128 /* advertised as SHMMNI, so the table limit is not a surprise */
#define SYSV_SHM_SHMMAX     (32UL * 1024 * 1024 * 1024) /* generous: guest RAM bounds the truth */
#define MAX_SYSV_SHM_ATTACH 256 /* advertised as SHMSEG */
#define SYSV_SHM_PAGE_SIZE  0x1000 /* Linux checks `shmat()` addresses against the CPU page size */

struct sysv_shm_seg {
    bool used;
    bool removed;
    int key;
    unsigned short seq;
    size_t size;
    int fd; /* fd in the application table, opened O_RDWR; the only reference to the file */
    unsigned short mode; /* IPC permission bits (0777), enforced from this table */
    bool locked; /* SHM_LOCK held; published as the SHM_LOCKED bit of shm_perm.mode */
    unsigned int uid, gid, cuid, cgid;
    int cpid, lpid;
    long atime, dtime, ctime; /* seconds, CLOCK_REALTIME, as in `struct shmid_ds` */
    size_t nattach;
};

struct sysv_shm_attach {
    bool used;
    uintptr_t addr;
    size_t len;
    int shmid;
};

static struct libos_lock g_sysv_shm_lock;
static struct sysv_shm_seg g_sysv_shm_segs[MAX_SYSV_SHM_SEGS];
static struct sysv_shm_attach g_sysv_shm_attaches[MAX_SYSV_SHM_ATTACH];

int init_sysv_shm(void) {
    return create_lock(&g_sysv_shm_lock) ? 0 : -ENOMEM;
}

static long shm_now_seconds(void) {
    uint64_t us = 0;
    if (PalSystemTimeQuery(&us) < 0)
        return 0;
    return (long)(us / 1000000ULL);
}

/* the permission bits of the class (`owner`/`group`/`other`) that `euid`/`egid` fall in */
static unsigned short shm_class_bits(const struct sysv_shm_seg* seg) {
    unsigned int euid = (unsigned int)libos_syscall_geteuid();
    unsigned int egid = (unsigned int)libos_syscall_getegid();
    if (euid == 0 || euid == seg->uid || euid == seg->cuid)
        return (seg->mode >> 6) & 07;
    if (egid == seg->gid || egid == seg->cgid)
        return (seg->mode >> 3) & 07;
    return seg->mode & 07;
}

/* Permission check of `shmat()`: the caller needs read, and unless SHM_RDONLY, write as well;
 * root (or a matching IPC owner) uses the owner bits, a matching group the group bits. */
static bool shm_may_attach(const struct sysv_shm_seg* seg, bool rdonly) {
    unsigned short need = rdonly ? 04 : 06;
    return (shm_class_bits(seg) & need) == need;
}

/*
 * Which backing directory actually provides a shared file mapping is a PAL property: the direct
 * PAL maps host files (`/tmp`, via `PalStreamMap`) and two attaches then share pages live; the VM
 * PAL cannot map host files `MAP_SHARED` and must fall back to the LibOS tmpfs (`/dev/shm`),
 * whose `mmap` is emulated (contents are copied into the new mapping — hence the flush of the
 * existing mappings in `shmat()`, so a new attach at least sees the current data, though not
 * later live writes; recorded in dev docs: libos-syscalls/TODO.md). Probe once.
 */
static const char* const g_backing_dirs[] = {"/tmp", "/dev/shm"};
static int g_backing_dir = -1; /* index into g_backing_dirs once the probe succeeded */

static int shm_pick_backing_dir(uint64_t probe_id) {
    if (g_backing_dir >= 0)
        return g_backing_dir;
    int creatable = -1; /* a directory we may create in, even if the mapping probe failed there */
    for (size_t i = 0; i < ARRAY_SIZE(g_backing_dirs); i++) {
        char path[96];
        int n = snprintf(path, sizeof(path), "%s/.gramine-sysv-shm-probe-%llu",
                         g_backing_dirs[i], (unsigned long long)probe_id);
        if (n < 0 || (size_t)n >= sizeof(path))
            continue;
        long fd = libos_openat_internal(AT_FDCWD, path, O_RDWR | O_CREAT | O_EXCL, 0666);
        if (fd < 0) {
            /* a directory this process may not create in is not a PAL property; skip it quietly */
            log_debug("sysv shm probe: cannot create in %s: %d", path, (int)fd);
            continue;
        }
        bool can_create = libos_syscall_ftruncate((int)fd, 4096) == 0;
        if (can_create)
            creatable = creatable < 0 ? (int)i : creatable;
        bool can_map = false;
        void* p = NULL;
        if (can_create) {
            p = libos_syscall_mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, (int)fd, 0);
            long pv = (long)p;
            can_map = !(pv < 0 && pv >= -4095); /* else: an errno in the error range */
            if (can_map)
                libos_syscall_munmap(p, 4096);
        }
        libos_syscall_close((int)fd);
        libos_unlink_internal(path);
        if (can_map) {
            /* cache only a confirmed working combination (PAL property, credential independent) */
            g_backing_dir = (int)i;
            return g_backing_dir;
        }
    }
    /* Nothing mapped `MAP_SHARED` under the current credentials: still create the segment where
     * creation works (a later `shmat()` reports what the PAL says about the mapping itself). Do
     * not cache this: another credential set may well map where this one could not. */
    if (creatable >= 0)
        return creatable;
    return -ENOENT;
}

long libos_syscall_shmget(int key, size_t size, int shmflg) {
    /* --- guest-wide System V IPC (VM, sys.experimental__multiprocess): libos_sysv_shared.c --- */
    if (sysv_shared_enabled())
        return sysv_shared_shmget(key, size, shmflg);
    /* --- end of guest-wide System V IPC --- */
    if (shmflg & LINUX_SHM_HUGETLB)
        return -EPERM; /* no hugetlb backing, as for memfd_create's MFD_HUGETLB */
    if (shmflg & ~(LINUX_IPC_CREAT | LINUX_IPC_EXCL | 0777 | LINUX_SHM_NORESERVE))
        return -EINVAL;
    if (size > SYSV_SHM_SHMMAX)
        return -EINVAL;

    lock(&g_sysv_shm_lock);

    if (key != LINUX_IPC_PRIVATE) {
        for (size_t i = 0; i < MAX_SYSV_SHM_SEGS; i++) {
            struct sysv_shm_seg* seg = &g_sysv_shm_segs[i];
            if (!seg->used || seg->removed || seg->key != key)
                continue;
            if ((shmflg & (LINUX_IPC_CREAT | LINUX_IPC_EXCL))
                    == (LINUX_IPC_CREAT | LINUX_IPC_EXCL)) {
                unlock(&g_sysv_shm_lock);
                return -EEXIST;
            }
            if (size && size > seg->size) {
                unlock(&g_sysv_shm_lock);
                return -EINVAL;
            }
            /* Linux checks the requested access (the permission bits given in `shmflg`)
             * against the segment's IPC permissions when finding an existing key; like the
             * kernel's ipcperms(), the requested bits of every class are folded into one 3-bit
             * class to compare with the caller's class bits. */
            unsigned short need = (unsigned short)(((shmflg >> 6) | (shmflg >> 3) | shmflg) & 07);
            if ((shm_class_bits(seg) & need) != need) {
                unlock(&g_sysv_shm_lock);
                return -EACCES;
            }
            unlock(&g_sysv_shm_lock);
            return (long)i;
        }
    }

    if (!(shmflg & LINUX_IPC_CREAT) && key != LINUX_IPC_PRIVATE) {
        /* IPC_PRIVATE never matches an existing key and Linux therefore creates with it even
         * without IPC_CREAT (LTP shmat03 relies on that). */
        unlock(&g_sysv_shm_lock);
        return -ENOENT;
    }
    if (!size) {
        unlock(&g_sysv_shm_lock);
        return -EINVAL;
    }

    size_t free_index = MAX_SYSV_SHM_SEGS;
    for (size_t i = 0; i < MAX_SYSV_SHM_SEGS; i++) {
        if (!g_sysv_shm_segs[i].used) {
            free_index = i;
            break;
        }
    }
    if (free_index == MAX_SYSV_SHM_SEGS) {
        unlock(&g_sysv_shm_lock);
        return -ENOSPC;
    }

    /* Back the segment by a fresh host file, then unlink it: the fd keeps it alive and a process
     * that dies without IPC_RMID still leaks nothing. The name mirrors the memfd backing files. */
    static uint64_t next_shm_id;
    uint64_t id = __atomic_add_fetch(&next_shm_id, 1, __ATOMIC_RELAXED);
    int dir = shm_pick_backing_dir(id);
    if (dir < 0) {
        unlock(&g_sysv_shm_lock);
        return dir;
    }
    char path[96];
    long fd = -1;
    for (size_t attempt = 0; attempt < ARRAY_SIZE(g_backing_dirs); attempt++) {
        size_t i = (size_t)dir + attempt < ARRAY_SIZE(g_backing_dirs)
                       ? (size_t)dir + attempt
                       : (size_t)dir + attempt - ARRAY_SIZE(g_backing_dirs);
        int n = snprintf(path, sizeof(path), "%s/.gramine-sysv-shm-%u-%llu", g_backing_dirs[i],
                         g_process.pid, (unsigned long long)id);
        if (n < 0 || (size_t)n >= sizeof(path)) {
            unlock(&g_sysv_shm_lock);
            return -ENAMETOOLONG;
        }
        /* The backing file's host permissions are not the segment's IPC permissions: the host
         * uid never follows the application's emulated credentials, so a mode from `shmflg`
         * would make the LibOS DAC check on the host-backed file reject creation or reopen of
         * one's own segment (LTP shmget04/shmctl02 create mode-0/0600 segments as nobody).
         * Permissions are enforced from this table instead; the file gets the generous mode and
         * is unlinked immediately below. */
        fd = libos_openat_internal(AT_FDCWD, path, O_RDWR | O_CREAT | O_EXCL, 0666);
        if (fd >= 0)
            break;
        if (fd != -ENOENT && fd != -EROFS && fd != -EACCES) {
            unlock(&g_sysv_shm_lock);
            return fd;
        }
    }
    if (fd < 0) {
        unlock(&g_sysv_shm_lock);
        return fd;
    }

    long ret = libos_syscall_ftruncate((int)fd, (loff_t)size);
    if (ret < 0)
        goto err_fd;
    ret = libos_unlink_internal(path);
    if (ret < 0)
        goto err_fd;

    struct sysv_shm_seg* seg = &g_sysv_shm_segs[free_index];
    assert(!seg->used);
    *seg = (struct sysv_shm_seg){
        .used  = true,
        .key   = key == LINUX_IPC_PRIVATE ? 0 : key,
        .seq   = (unsigned short)id,
        .size  = size,
        .fd    = (int)fd,
        .mode  = (unsigned short)(shmflg & 0777),
        .uid   = (unsigned int)libos_syscall_getuid(),
        .gid   = (unsigned int)libos_syscall_getgid(),
        .cuid  = (unsigned int)libos_syscall_geteuid(),
        .cgid  = (unsigned int)libos_syscall_getegid(),
        .cpid  = g_process.pid,
        .lpid  = g_process.pid,
        .atime = 0,
        .dtime = 0,
        .ctime = shm_now_seconds(),
    };
    unlock(&g_sysv_shm_lock);
    return (long)free_index;

err_fd:
    libos_syscall_close((int)fd);
    libos_unlink_internal(path);
    unlock(&g_sysv_shm_lock);
    return ret;
}

long libos_syscall_shmat(int shmid, const void* shmaddr, int shmflg) {
    /* --- guest-wide System V IPC (VM, sys.experimental__multiprocess): libos_sysv_shared.c --- */
    if (sysv_shared_enabled())
        return sysv_shared_shmat(shmid, shmaddr, shmflg);
    /* --- end of guest-wide System V IPC --- */
    if (shmflg & ~(LINUX_SHM_RDONLY | LINUX_SHM_RND | LINUX_SHM_REMAP | LINUX_SHM_EXEC))
        return -EINVAL;

    lock(&g_sysv_shm_lock);
    struct sysv_shm_seg* seg = NULL;
    if (shmid >= 0 && shmid < MAX_SYSV_SHM_SEGS) {
        seg = &g_sysv_shm_segs[shmid];
        if (!seg->used || seg->removed)
            seg = NULL;
    }
    if (!seg) {
        unlock(&g_sysv_shm_lock);
        return -EINVAL;
    }

    if (!shm_may_attach(seg, shmflg & LINUX_SHM_RDONLY)) {
        unlock(&g_sysv_shm_lock);
        return -EACCES;
    }

    /* Linux rejects an unaligned non-null address unless it is honored (SHM_REMAP) or rounded
     * (SHM_RND); SHM_RND rounds down to SHMLBA, SHM_REMAP takes the address exactly. */
    uintptr_t addr = (uintptr_t)shmaddr;
    if (addr) {
        if (shmflg & LINUX_SHM_RND)
            addr &= ~(uintptr_t)(LINUX_SHMLBA - 1);
        if (!(shmflg & (LINUX_SHM_RND | LINUX_SHM_REMAP)) && (addr % SYSV_SHM_PAGE_SIZE)) {
            unlock(&g_sysv_shm_lock);
            return -EINVAL;
        }
    }

    int prot = PROT_READ;
    if (!(shmflg & LINUX_SHM_RDONLY))
        prot |= PROT_WRITE;
    if (shmflg & LINUX_SHM_EXEC)
        prot |= PROT_EXEC;

    int flags = MAP_SHARED | ((shmflg & LINUX_SHM_REMAP) ? MAP_FIXED : 0);
    size_t size = seg->size;
    int fd = seg->fd;

    /* Flush the current mappings before the new one is created: on the tmpfs backing the new
     * mapping copies the file contents, and flushed mappings are the only way to carry the data
     * written through them so far. */
    uintptr_t flush_begin[8];
    uintptr_t flush_end[8];
    size_t nflush = 0;
    for (size_t i = 0; i < MAX_SYSV_SHM_ATTACH && nflush < ARRAY_SIZE(flush_begin); i++) {
        if (g_sysv_shm_attaches[i].used && g_sysv_shm_attaches[i].shmid == shmid) {
            flush_begin[nflush] = g_sysv_shm_attaches[i].addr;
            flush_end[nflush] = g_sysv_shm_attaches[i].addr + g_sysv_shm_attaches[i].len;
            nflush++;
        }
    }
    unlock(&g_sysv_shm_lock);

    for (size_t i = 0; i < nflush; i++)
        msync_range(flush_begin[i], flush_end[i]);

    void* ret = libos_syscall_mmap((void*)addr, size, prot, flags, fd, 0);
    if ((long)ret < 0 && (long)ret >= -4095)
        return (long)ret; /* EBADF here means the application closed the segment's internal fd */

    lock(&g_sysv_shm_lock);
    seg = (shmid >= 0 && shmid < MAX_SYSV_SHM_SEGS) ? &g_sysv_shm_segs[shmid] : NULL;
    if (!seg || !seg->used || seg->removed) {
        unlock(&g_sysv_shm_lock);
        libos_syscall_munmap(ret, size);
        return -EINVAL;
    }
    struct sysv_shm_attach* attach = NULL;
    for (size_t i = 0; i < MAX_SYSV_SHM_ATTACH; i++) {
        if (!g_sysv_shm_attaches[i].used) {
            attach = &g_sysv_shm_attaches[i];
            break;
        }
    }
    if (!attach) {
        unlock(&g_sysv_shm_lock);
        /* No room to remember the range: unmap rather than leak a mapping `shmdt()` could not
         * find; Linux has no equivalent limit. */
        libos_syscall_munmap(ret, size);
        return -ENOMEM;
    }
    *attach = (struct sysv_shm_attach){.used = true, .addr = (uintptr_t)ret, .len = size,
                                       .shmid = shmid};
    seg->nattach++;
    seg->atime = shm_now_seconds();
    seg->lpid = g_process.pid;
    unlock(&g_sysv_shm_lock);
    return (long)ret;
}

long libos_syscall_shmdt(const void* shmaddr) {
    /* --- guest-wide System V IPC (VM, sys.experimental__multiprocess): libos_sysv_shared.c --- */
    if (sysv_shared_enabled())
        return sysv_shared_shmdt(shmaddr);
    /* --- end of guest-wide System V IPC --- */
    uintptr_t addr = (uintptr_t)shmaddr;
    if (!addr || (addr % SYSV_SHM_PAGE_SIZE))
        return -EINVAL;

    lock(&g_sysv_shm_lock);
    struct sysv_shm_attach* attach = NULL;
    for (size_t i = 0; i < MAX_SYSV_SHM_ATTACH; i++) {
        struct sysv_shm_attach* a = &g_sysv_shm_attaches[i];
        if (a->used && a->addr == addr) {
            attach = a;
            break;
        }
    }
    if (!attach) {
        unlock(&g_sysv_shm_lock);
        return -EINVAL;
    }
    size_t len = attach->len;
    int shmid = attach->shmid;
    attach->used = false;

    struct sysv_shm_seg* seg = (shmid >= 0 && shmid < MAX_SYSV_SHM_SEGS)
                                   ? &g_sysv_shm_segs[shmid] : NULL;
    if (seg && seg->used) {
        assert(seg->nattach);
        seg->nattach--;
        seg->dtime = shm_now_seconds();
        seg->lpid = g_process.pid;
    }
    bool free_seg = seg && seg->used && seg->removed && !seg->nattach;
    int fd = free_seg ? seg->fd : -1;
    if (free_seg)
        *seg = (struct sysv_shm_seg){0};
    unlock(&g_sysv_shm_lock);

    long ret = libos_syscall_munmap((void*)addr, len);
    if (ret < 0)
        return ret;
    if (fd >= 0)
        libos_syscall_close(fd);
    return 0;
}

static void shm_fill_ds(const struct sysv_shm_seg* seg, struct linux_shmid_ds* ds) {
    *ds = (struct linux_shmid_ds){
        .shm_perm = {
            .key  = seg->key,
            .uid  = seg->uid,
            .gid  = seg->gid,
            .cuid = seg->cuid,
            .cgid = seg->cgid,
            .mode = (unsigned short)(seg->mode | (seg->locked ? LINUX_SHM_LOCKED : 0)),
            .seq  = seg->seq,
        },
        .shm_segsz  = seg->size,
        .shm_atime  = seg->atime,
        .shm_dtime  = seg->dtime,
        .shm_ctime  = seg->ctime,
        .shm_cpid   = seg->cpid,
        .shm_lpid   = seg->lpid,
        .shm_nattch = seg->nattach,
    };
}

long libos_syscall_shmctl(int shmid, int cmd, void* buf) {
    /* --- guest-wide System V IPC (VM, sys.experimental__multiprocess): libos_sysv_shared.c --- */
    if (sysv_shared_enabled())
        return sysv_shared_shmctl(shmid, cmd, buf);
    /* --- end of guest-wide System V IPC --- */
    switch (cmd) {
        case LINUX_IPC_INFO: {
            if (!buf)
                return -EFAULT;
            if (!is_user_memory_writable(buf, sizeof(struct linux_shminfo64)))
                return -EFAULT;
            struct linux_shminfo64* info = buf;
            *info = (struct linux_shminfo64){
                .shmmax = SYSV_SHM_SHMMAX,
                .shmin  = 1,
                .shmmni = MAX_SYSV_SHM_SEGS,
                .shmseg = MAX_SYSV_SHM_ATTACH,
                .shmall = SYSV_SHM_SHMMAX,
            };
            return 0;
        }
        case LINUX_SHM_INFO: {
            if (!buf)
                return -EFAULT;
            if (!is_user_memory_writable(buf, sizeof(struct linux_shm_info)))
                return -EFAULT;
            struct linux_shm_info info = {0};
            long ret = -EINVAL;
            lock(&g_sysv_shm_lock);
            for (size_t i = 0; i < MAX_SYSV_SHM_SEGS; i++) {
                if (g_sysv_shm_segs[i].used && !g_sysv_shm_segs[i].removed) {
                    info.used_ids++;
                    /* pages of live segments, as the kernel's `shm_tot`; `shm_rss`/`shm_swp`
                     * stay 0 because the PAL reports no residency (matching the 0 columns of
                     * `/proc/sysvipc/shm` below) */
                    info.shm_tot += (g_sysv_shm_segs[i].size + SYSV_SHM_PAGE_SIZE - 1)
                                    / SYSV_SHM_PAGE_SIZE;
                }
            }
            /* `shmid` indexes the identifiers, as on Linux, and the command answers with the
             * key of the indexed segment; the accounting fields stay zero because the PAL
             * reports no page residency. */
            size_t nth = 0;
            for (size_t i = 0; i < MAX_SYSV_SHM_SEGS; i++) {
                if (!g_sysv_shm_segs[i].used || g_sysv_shm_segs[i].removed)
                    continue;
                if (nth++ == (size_t)shmid) {
                    ret = g_sysv_shm_segs[i].key;
                    break;
                }
            }
            unlock(&g_sysv_shm_lock);
            if (ret == -EINVAL)
                return ret;
            *(struct linux_shm_info*)buf = info;
            return ret;
        }
        case LINUX_IPC_STAT:
        case LINUX_SHM_STAT:
        case LINUX_SHM_STAT_ANY:
            if (!buf)
                return -EFAULT;
            if (!is_user_memory_writable(buf, sizeof(struct linux_shmid_ds)))
                return -EFAULT;
            break;
        case LINUX_IPC_SET:
            if (!buf)
                return -EFAULT;
            if (!is_user_memory_readable(buf, sizeof(struct linux_shmid_ds)))
                return -EFAULT;
            break;
        case LINUX_IPC_RMID:
        case LINUX_SHM_LOCK:
        case LINUX_SHM_UNLOCK:
            break;
        default:
            return -EINVAL;
    }

    lock(&g_sysv_shm_lock);

    struct sysv_shm_seg* seg = NULL;
    if (cmd == LINUX_SHM_STAT || cmd == LINUX_SHM_STAT_ANY) {
        /* `shmid` is an index into the identifiers, not an identifier itself. SHM_STAT hides
         * segments the caller could not read; per-caller visibility is not modeled here, so
         * SHM_STAT and SHM_STAT_ANY behave alike. */
        if (shmid < 0) {
            unlock(&g_sysv_shm_lock);
            return -EINVAL;
        }
        size_t nth = 0;
        for (size_t i = 0; i < MAX_SYSV_SHM_SEGS; i++) {
            if (!g_sysv_shm_segs[i].used || g_sysv_shm_segs[i].removed)
                continue;
            if (nth++ == (size_t)shmid) {
                seg = &g_sysv_shm_segs[i];
                break;
            }
        }
    } else if (shmid >= 0 && shmid < MAX_SYSV_SHM_SEGS) {
        seg = &g_sysv_shm_segs[shmid];
        if (!seg->used || seg->removed)
            seg = NULL;
    }
    if (!seg) {
        unlock(&g_sysv_shm_lock);
        return -EINVAL;
    }

    long ret = 0;
    switch (cmd) {
        case LINUX_IPC_STAT:
        case LINUX_SHM_STAT:
            /* Linux requires read permission on the segment to inspect it (`ipcperms` with
             * S_IRUGO); SHM_STAT_ANY is explicitly the variant without that check. */
            if ((shm_class_bits(seg) & 04) != 04) {
                unlock(&g_sysv_shm_lock);
                return -EACCES;
            }
            /* fall through */
        case LINUX_SHM_STAT_ANY: {
            struct linux_shmid_ds ds;
            shm_fill_ds(seg, &ds);
            /* SHM_STAT answers with the identifier, IPC_STAT with success (0). */
            ret = cmd == LINUX_IPC_STAT ? 0 : seg - g_sysv_shm_segs;
            unlock(&g_sysv_shm_lock);
            *(struct linux_shmid_ds*)buf = ds;
            break;
        }
        case LINUX_IPC_SET: {
            struct linux_shmid_ds* ds = buf;
            unsigned int euid = (unsigned int)libos_syscall_geteuid();
            if (euid != 0 && euid != seg->cuid && euid != seg->uid) {
                unlock(&g_sysv_shm_lock);
                return -EPERM;
            }
            seg->uid = ds->shm_perm.uid;
            seg->gid = ds->shm_perm.gid;
            seg->mode = (unsigned short)(ds->shm_perm.mode & 0777);
            seg->ctime = shm_now_seconds();
            seg->lpid = g_process.pid;
            unlock(&g_sysv_shm_lock);
            break;
        }
        case LINUX_IPC_RMID:
            /* Only the creator, the current owner, or root may remove the identifier. */
            if ((unsigned int)libos_syscall_geteuid() != 0
                    && (unsigned int)libos_syscall_geteuid() != seg->cuid
                    && (unsigned int)libos_syscall_geteuid() != seg->uid) {
                unlock(&g_sysv_shm_lock);
                return -EPERM;
            }
            /* Linux destroys the identifier now and the segment when the last attach detaches
             * (or immediately if nobody is attached). */
            seg->removed = true;
            seg->lpid = g_process.pid;
            seg->ctime = shm_now_seconds();
            if (!seg->nattach) {
                int fd = seg->fd;
                *seg = (struct sysv_shm_seg){0};
                unlock(&g_sysv_shm_lock);
                libos_syscall_close(fd);
            } else {
                unlock(&g_sysv_shm_lock);
            }
            break;
        case LINUX_SHM_LOCK:
        case LINUX_SHM_UNLOCK:
            /* Locking needs CAP_IPC_LOCK, i.e. root here (LTP shmctl02 checks the EPERM).
             * Page residency is not tracked (the PAL has no page cache to lock); the command
             * is accepted and only the SHM_LOCKED bit of `shm_perm.mode` is published. */
            if ((unsigned int)libos_syscall_geteuid() != 0) {
                unlock(&g_sysv_shm_lock);
                return -EPERM;
            }
            seg->locked = cmd == LINUX_SHM_LOCK;
            seg->lpid = g_process.pid;
            unlock(&g_sysv_shm_lock);
            break;
        default:
            unlock(&g_sysv_shm_lock);
            return -EINVAL;
    }
    return ret;
}

/* Content of `/proc/sysvipc/shm`: the header line and one line per live segment, in the field
 * order the kernel uses (`key shmid perms size cpid lpid nattch uid gid cuid cgid atime dtime
 * ctime rss swap`). The PAL reports no page residency, so `rss` and `swap` read 0 — which is what
 * a Linux kernel with unswapped segments shows, and what `shmctl(SHM_INFO)` reports here, so the
 * two views stay consistent (the check LTP shmctl04 performs). */
int sysv_shm_proc_load(char** out_data, size_t* out_size) {
    /* --- guest-wide System V IPC (VM, sys.experimental__multiprocess): libos_sysv_shared.c --- */
    if (sysv_shared_enabled())
        return sysv_shared_shm_proc_load(out_data, out_size);
    /* --- end of guest-wide System V IPC --- */
    static const char header[] =
        "key      shmid perms                  size  cpid  lpid nattch   uid   gid  cuid  cgid"
        "      atime      dtime      ctime                   rss                  swap\n";
    size_t cap = sizeof(header) + MAX_SYSV_SHM_SEGS * 160;
    char* buf = malloc(cap);
    if (!buf)
        return -ENOMEM;
    size_t off = sizeof(header) - 1;
    memcpy(buf, header, off);

    lock(&g_sysv_shm_lock);
    for (size_t i = 0; i < MAX_SYSV_SHM_SEGS; i++) {
        struct sysv_shm_seg* seg = &g_sysv_shm_segs[i];
        if (!seg->used || seg->removed)
            continue;
        int n = snprintf(buf + off, cap - off,
                         "%10d %10zu %5o %20zu %5d %5d %4zu %5u %5u %5u %5u %10ld %10ld %10ld"
                         " %16lu %16lu\n",
                         seg->key, i, seg->mode, seg->size, seg->cpid, seg->lpid, seg->nattach,
                         seg->uid, seg->gid, seg->cuid, seg->cgid, seg->atime, seg->dtime,
                         seg->ctime, 0UL, 0UL);
        assert(n > 0 && (size_t)n < cap - off);
        off += (size_t)n;
    }
    unlock(&g_sysv_shm_lock);

    *out_data = buf;
    *out_size = off;
    return 0;
}
