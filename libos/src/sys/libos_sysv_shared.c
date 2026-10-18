/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/*
 * System V shared memory, message queues and semaphores shared by all processes of one guest
 * (x86_64 VM PAL with `sys.experimental__multiprocess`).
 *
 * The process-local implementations (libos_sysv_shm.c, libos_sysv_msg.c, libos_sem.c) keep their
 * tables in LibOS memory, so a forked child starts with empty tables. With the multiprocess support
 * every entry point of those files hands over to this file instead (one block at its top, see
 * libos_sysv_shared.h). The semantics follow those files (the kernel's check order, identifiers,
 * `ipcperms()`, the IPC_STAT/INFO layouts and limits); what differs is where the state lives:
 *
 *   - One area in the guest's POSIX shared-memory store (`dev:/dev/shm/.gramine-sysv-ipc`, served
 *     by the VM PAL in guest RAM and shared by every process) holds all tables. Each process maps
 *     it at its own address at first use, so the area holds no pointers: lists and variable-size
 *     data (messages, semaphore arrays) are offsets into a small heap at its end.
 *   - A futex mutex in the area guards everything; waiters (msgsnd/msgrcv on a full or empty queue,
 *     semop) sleep on the object's sequence word with the process-shared futexes of the VM PAL
 *     (PalFutexShared*) and every change of the object bumps and wakes it.
 *   - A segment is backed by a named file in the guest store (`/dev/shm`) or, without that mount,
 *     in `/tmp` (whose shared mappings the VM PAL also shares between processes); an attach opens
 *     it, maps it and closes the descriptor. Attachments are recorded per process id, so a forked
 *     child inherits its parent's, and the exit of a process detaches its own and applies its
 *     SEM_UNDO adjustments.
 *
 * Limits: those of the process-local files, and a heap of SYSV_SHARED_HEAP_SIZE for all messages
 * and semaphore arrays together (a message that does not fit waits like one that exceeds its
 * queue's limit, a set that does not fit gives ENOSPC). A process that dies while holding the area
 * mutex (a crash inside these functions) leaves it locked for the others.
 */

#include "api.h"
#include "libos_fs.h"
#include "libos_internal.h"
#include "libos_lock.h"
#include "libos_process.h"
#include "libos_sysv_shared.h"
#include "libos_table.h"
#include "libos_thread.h"
#include "libos_utils.h"
#include "libos_vma.h"
#include "linux_abi/errors.h"
#include "linux_abi/fs.h"
#include "linux_abi/ipc.h"
#include "linux_abi/memory.h"
#include "pal.h"
#include "toml_utils.h"

/* the limits of the process-local implementations */
#define SHM_SEGS        128
#define SHM_ATTACH      256 /* per process, advertised as SHMSEG */
#define SHM_ATTACH_ALL  2048
#define SHM_SHMMAX      (32UL * 1024 * 1024 * 1024)
#define SHM_PAGE_SIZE   0x1000
#define MSG_MNI         1024
#define MSG_MAX         8192
#define MSG_MNB         16384
#define MSG_IDX_BITS    15
#define SEM_MNI         1024
#define SEM_MSL         32000
#define SEM_OPM         500
#define SEM_VMX         32767
#define SEM_MNS         (SEM_MNI * SEM_MSL)
#define SEM_IDX_BITS    15
#define SEM_UNDO_ALL    4096

#define SYSV_SHARED_HEAP_SIZE (6UL * 1024 * 1024)
#define SYSV_SHARED_MAGIC     0x5359535653484152UL /* "SYSVSHAR" */
#define SYSV_SHARED_URI       "dev:/dev/shm/.gramine-sysv-ipc"

/* semctl commands and flags (the process-local file defines them itself) */
#define SEM_UNDO     010000
#define SEM_GETPID   11
#define SEM_GETVAL   12
#define SEM_GETALL   13
#define SEM_GETNCNT  14
#define SEM_GETZCNT  15
#define SEM_SETVAL   16
#define SEM_SETALL   17
#define SEM_STAT     18
#define SEM_INFO     19
#define SEM_STAT_ANY 20

struct shared_sembuf { /* the layout of `struct sembuf` */
    unsigned short sem_num;
    short sem_op;
    short sem_flg;
};

struct shared_ipc64_perm {
    int key;
    unsigned int uid, gid, cuid, cgid;
    unsigned short mode, pad1;
    unsigned short seq, pad2;
    unsigned long unused1, unused2;
};

struct shared_semid64_ds {
    struct shared_ipc64_perm sem_perm;
    long sem_otime;
    SYSV_SEMID64_PAD(unused1)
    long sem_ctime;
    SYSV_SEMID64_PAD(unused2)
    unsigned long sem_nsems;
    unsigned long unused3, unused4;
};

struct shared_seminfo {
    int semmap, semmni, semmns, semmnu, semmsl, semopm, semume, semusz, semvmx, semaem;
};

typedef uint32_t heap_off_t; /* offset from the area start; 0 is "none" */

struct shm_seg {
    bool used;
    bool removed;
    bool locked;
    int key;
    unsigned short seq;
    size_t size;
    unsigned short mode;
    unsigned int uid, gid, cuid, cgid;
    int cpid, lpid;
    long atime, dtime, ctime;
    size_t nattach;
    char path[96]; /* the backing file, as the LibOS names it */
};

struct shm_attach {
    IDTYPE pid; /* 0: free */
    uintptr_t addr;
    size_t len;
    int shmid;
};

struct msg_hdr {
    heap_off_t next;
    long type;
    size_t len;
    char text[];
};

struct msq {
    bool used;
    int key;
    unsigned short seq;
    unsigned short mode;
    unsigned int uid, gid, cuid, cgid;
    long stime, rtime, ctime;
    size_t cbytes, qnum, qbytes;
    int lspid, lrpid;
    heap_off_t head, tail;
    uint32_t wseq; /* futex word: bumped and woken on every change */
};

struct sem_value {
    unsigned short value;
    int pid;
    uint32_t ncnt, zcnt; /* waiters for an increase / for zero */
};

struct sem_set {
    bool used;
    int key;
    unsigned short seq;
    unsigned short mode;
    unsigned int uid, gid, cuid, cgid;
    long otime, ctime;
    int nsems;
    heap_off_t sems; /* struct sem_value[nsems] */
    uint32_t wseq;
};

struct sem_undo {
    IDTYPE pid; /* 0: free */
    unsigned int index;
    unsigned short seq;
    unsigned short semnum;
    int adj;
};

struct heap_block {
    uint32_t size; /* of the whole block, header included */
    heap_off_t next_free;
};

struct sysv_area {
    uint64_t magic;
    uint32_t lock; /* futex mutex: 0 free, 1 locked, 2 locked with waiters */
    uint32_t token; /* names the segment files of this guest */
    uint64_t next_file; /* generation of segment files */
    unsigned short msg_next_seq[MSG_MNI];
    unsigned short sem_next_seq[SEM_MNI];
    size_t msg_in_use, sem_in_use;
    struct shm_seg segs[SHM_SEGS];
    struct shm_attach attach[SHM_ATTACH_ALL];
    struct msq msqs[MSG_MNI];
    struct sem_set sets[SEM_MNI];
    struct sem_undo undo[SEM_UNDO_ALL];
    heap_off_t heap_free;
    size_t heap_start, heap_end;
};

#define SYSV_AREA_SIZE ALLOC_ALIGN_UP(sizeof(struct sysv_area) + SYSV_SHARED_HEAP_SIZE)

static bool g_enabled;
static bool g_sem_enabled;
static struct sysv_area* g_area;
static struct libos_lock g_area_map_lock; /* the first mapping of the area in this process */

bool sysv_shared_enabled(void) {
    return g_enabled;
}

bool sysv_shared_sem_enabled(void) {
    return g_sem_enabled;
}

int init_sysv_shared(void) {
    /* on by default where the VM PAL has the frame pool (x86_64, aarch64); see pal_main.c */
#if defined(__x86_64__) || defined(__aarch64__)
    const bool default_multiprocess = true;
#else
    const bool default_multiprocess = false;
#endif
    bool multiprocess = default_multiprocess;
    int ret = toml_bool_in(g_manifest_root, "sys.experimental__multiprocess",
                           /*defaultval=*/default_multiprocess, &multiprocess);
    if (ret < 0)
        return 0; /* the PAL rejects a malformed value */
    g_enabled = multiprocess && !strcmp(g_pal_public_state->host_type, "VM");
    bool host_semaphores = false;
    (void)toml_bool_in(g_manifest_root, "sys.experimental__enable_host_sysv_semaphores",
                       /*defaultval=*/false, &host_semaphores);
    g_sem_enabled = g_enabled && !host_semaphores;
    if (g_enabled && !create_lock(&g_area_map_lock))
        return -ENOMEM;
    return 0;
}

/* --- futex helpers on the area (all processes map the same frames) --- */

static int area_wait(uint32_t* word, uint32_t val, uint64_t* timeout_us, bool interruptible) {
    thread_prepare_wait();
    void* waiter = NULL;
    int ret = PalFutexSharedAdd(word, val, /*bitset=*/0xffffffff, get_cur_thread()->scheduler_event,
                                &waiter);
    if (ret == -PAL_ERROR_TRYAGAIN)
        return 0;
    if (ret < 0)
        return pal_to_unix_errno(ret);
    ret = thread_wait(timeout_us, /*ignore_pending_signals=*/!interruptible);
    bool woken = false;
    (void)PalFutexSharedRemove(waiter, &woken);
    return woken ? 0 : ret;
}

static void area_wake(uint32_t* word) {
    int woken;
    (void)PalFutexSharedWake(word, /*bitset=*/0xffffffff, /*count=*/INT32_MAX, &woken);
}

static void area_lock(void) {
    uint32_t expected = 0;
    if (__atomic_compare_exchange_n(&g_area->lock, &expected, 1, /*weak=*/false, __ATOMIC_ACQUIRE,
                                    __ATOMIC_RELAXED))
        return;
    while (__atomic_exchange_n(&g_area->lock, 2, __ATOMIC_ACQUIRE) != 0)
        (void)area_wait(&g_area->lock, 2, /*timeout_us=*/NULL, /*interruptible=*/false);
}

static void area_unlock(void) {
    if (__atomic_exchange_n(&g_area->lock, 0, __ATOMIC_RELEASE) == 2)
        area_wake(&g_area->lock);
}

static void object_changed(uint32_t* wseq) {
    __atomic_add_fetch(wseq, 1, __ATOMIC_RELEASE);
    area_wake(wseq);
}

/* Unlocks the area and sleeps until `*wseq` changes from `seen` (or the timeout or a signal). */
static int area_unlock_and_wait(uint32_t* wseq, uint32_t seen, uint64_t* timeout_us) {
    area_unlock();
    return area_wait(wseq, seen, timeout_us, /*interruptible=*/true);
}

/* --- the area --- */

static void heap_init(struct sysv_area* area) {
    area->heap_start = ALIGN_UP(sizeof(*area), 16);
    area->heap_end = SYSV_AREA_SIZE;
    struct heap_block* block = (struct heap_block*)((char*)area + area->heap_start);
    block->size = (uint32_t)(area->heap_end - area->heap_start);
    block->next_free = 0;
    area->heap_free = (heap_off_t)area->heap_start;
}

static void* heap_ptr(heap_off_t off) {
    return off ? (char*)g_area + off : NULL;
}

static heap_off_t heap_alloc(size_t size) {
    size_t need = ALIGN_UP(size + sizeof(struct heap_block), 16);
    heap_off_t* link = &g_area->heap_free;
    while (*link) {
        struct heap_block* block = heap_ptr(*link);
        if (block->size >= need) {
            heap_off_t off = *link;
            if (block->size - need >= 64) {
                struct heap_block* rest = (struct heap_block*)((char*)block + need);
                rest->size = block->size - (uint32_t)need;
                rest->next_free = block->next_free;
                block->size = (uint32_t)need;
                *link = off + (heap_off_t)need;
            } else {
                *link = block->next_free;
            }
            return off + (heap_off_t)sizeof(struct heap_block);
        }
        link = &block->next_free;
    }
    return 0;
}

static void heap_free(heap_off_t off) {
    if (!off)
        return;
    heap_off_t block_off = off - (heap_off_t)sizeof(struct heap_block);
    struct heap_block* block = heap_ptr(block_off);
    /* keep the free list sorted by offset and merge neighbours */
    heap_off_t* link = &g_area->heap_free;
    while (*link && *link < block_off)
        link = &((struct heap_block*)heap_ptr(*link))->next_free;
    block->next_free = *link;
    *link = block_off;
    if (block->next_free && block_off + block->size == block->next_free) {
        struct heap_block* next = heap_ptr(block->next_free);
        block->size += next->size;
        block->next_free = next->next_free;
    }
    heap_off_t* prev_link = &g_area->heap_free;
    while (*prev_link && *prev_link != block_off) {
        struct heap_block* prev = heap_ptr(*prev_link);
        if (*prev_link + prev->size == block_off) {
            prev->size += block->size;
            prev->next_free = block->next_free;
            return;
        }
        prev_link = &prev->next_free;
    }
}

/* Maps the area at the first use in this process (creating and initializing it in the guest's
 * first process to use System V IPC). */
static int area_get(void) {
    if (__atomic_load_n(&g_area, __ATOMIC_ACQUIRE))
        return 0;
    lock(&g_area_map_lock);
    if (g_area) {
        unlock(&g_area_map_lock);
        return 0;
    }
    PAL_HANDLE handle = NULL;
    bool created = true;
    int ret = PalStreamOpen(SYSV_SHARED_URI, PAL_ACCESS_RDWR, /*share_flags=*/0600,
                            PAL_CREATE_ALWAYS, /*options=*/0, &handle);
    if (ret == -PAL_ERROR_STREAMEXIST) {
        created = false;
        ret = PalStreamOpen(SYSV_SHARED_URI, PAL_ACCESS_RDWR, /*share_flags=*/0600,
                            PAL_CREATE_NEVER, /*options=*/0, &handle);
    }
    if (ret < 0)
        goto out;
    if (created)
        ret = PalStreamSetLength(handle, SYSV_AREA_SIZE);
    if (ret < 0)
        goto out;
    void* addr = NULL;
    ret = bkeep_mmap_any(SYSV_AREA_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED | VMA_INTERNAL,
                         /*file=*/NULL, /*offset=*/0, "sysv-shared", &addr);
    if (ret < 0) {
        ret = -PAL_ERROR_NOMEM;
        goto out;
    }
    ret = PalStreamMap(handle, addr, PAL_PROT_READ | PAL_PROT_WRITE, /*offset=*/0, SYSV_AREA_SIZE);
    if (ret < 0) {
        void* tmp_vma = NULL;
        if (bkeep_munmap(addr, SYSV_AREA_SIZE, /*is_internal=*/true, &tmp_vma) == 0)
            bkeep_remove_tmp_vma(tmp_vma);
        goto out;
    }
    struct sysv_area* area = addr;
    if (created) {
        heap_init(area);
        uint32_t token = 0;
        (void)PalRandomBitsRead(&token, sizeof(token));
        area->token = token;
        __atomic_store_n(&area->magic, SYSV_SHARED_MAGIC, __ATOMIC_RELEASE);
    } else {
        /* another process is initializing it right now */
        while (__atomic_load_n(&area->magic, __ATOMIC_ACQUIRE) != SYSV_SHARED_MAGIC)
            PalThreadYieldExecution();
    }
    __atomic_store_n(&g_area, area, __ATOMIC_RELEASE);
    ret = 0;
out:
    unlock(&g_area_map_lock);
    /* the mapping keeps the object; the handle is not needed any more */
    if (handle)
        PalObjectDestroy(handle);
    if (ret < 0) {
        log_error("System V IPC shared between processes is not available: %s",
                  pal_strerror(ret));
        return -ENOMEM;
    }
    return 0;
}

static long now_seconds(void) {
    uint64_t us = 0;
    if (PalSystemTimeQuery(&us) < 0)
        return 0;
    return (long)(us / 1000000ULL);
}

static bool is_root(void) {
    return (unsigned int)libos_syscall_geteuid() == 0;
}

/* the kernel's `ipcperms()` */
static bool ipc_permitted(unsigned short mode, unsigned int uid, unsigned int gid,
                          unsigned int cuid, unsigned int cgid, unsigned int flag) {
    if (is_root())
        return true;
    unsigned int euid = (unsigned int)libos_syscall_geteuid();
    unsigned int egid = (unsigned int)libos_syscall_getegid();
    unsigned int requested = ((flag >> 6) | (flag >> 3) | flag) & 07;
    unsigned int granted = mode;
    if (euid == cuid || euid == uid)
        granted >>= 6;
    else if (egid == cgid || egid == gid)
        granted >>= 3;
    return !(requested & ~granted & 07);
}

static bool ipc_is_owner(unsigned int uid, unsigned int cuid) {
    unsigned int euid = (unsigned int)libos_syscall_geteuid();
    return is_root() || euid == cuid || euid == uid;
}

/*
 * Shared memory
 */

static unsigned short shm_class_bits(const struct shm_seg* seg) {
    unsigned int euid = (unsigned int)libos_syscall_geteuid();
    unsigned int egid = (unsigned int)libos_syscall_getegid();
    if (euid == 0 || euid == seg->uid || euid == seg->cuid)
        return (seg->mode >> 6) & 07;
    if (egid == seg->gid || egid == seg->cgid)
        return (seg->mode >> 3) & 07;
    return seg->mode & 07;
}

static struct shm_seg* shm_by_id(int shmid) {
    if (shmid < 0 || shmid >= SHM_SEGS)
        return NULL;
    struct shm_seg* seg = &g_area->segs[shmid];
    return seg->used && !seg->removed ? seg : NULL;
}

/* Frees a removed segment nobody is attached to; returns whether its file must be unlinked
 * (into `path`, after the area lock is dropped). */
static bool shm_maybe_destroy(struct shm_seg* seg, char* path, size_t path_size) {
    if (!seg->used || !seg->removed || seg->nattach)
        return false;
    snprintf(path, path_size, "%s", seg->path);
    *seg = (struct shm_seg){0};
    return true;
}

/* Creates the segment's backing file: in the guest store if `/dev/shm` is mounted, else `/tmp`.
 * Its mode is generous: the permissions are the segment's, enforced from the table. */
static int shm_create_file(size_t size, char* path, size_t path_size) {
    static const char* const dirs[] = {"/dev/shm", "/tmp"};
    uint64_t gen = __atomic_add_fetch(&g_area->next_file, 1, __ATOMIC_RELAXED);
    long ret = -ENOENT;
    for (size_t i = 0; i < ARRAY_SIZE(dirs); i++) {
        snprintf(path, path_size, "%s/.gramine-sysv-shm-%08x-%llu", dirs[i], g_area->token,
                 (unsigned long long)gen);
        long fd = libos_openat_internal(AT_FDCWD, path, O_RDWR | O_CREAT | O_EXCL, 0666);
        if (fd < 0) {
            ret = fd;
            continue;
        }
        ret = libos_syscall_ftruncate((int)fd, (loff_t)size);
        libos_syscall_close((int)fd);
        if (ret < 0) {
            libos_unlink_internal(path);
            continue;
        }
        return 0;
    }
    return (int)ret;
}

long sysv_shared_shmget(int key, size_t size, int shmflg) {
    if (shmflg & LINUX_SHM_HUGETLB)
        return -EPERM;
    if (shmflg & ~(LINUX_IPC_CREAT | LINUX_IPC_EXCL | 0777 | LINUX_SHM_NORESERVE))
        return -EINVAL;
    if (size > SHM_SHMMAX)
        return -EINVAL;
    int ret = area_get();
    if (ret < 0)
        return ret;

    area_lock();
    if (key != LINUX_IPC_PRIVATE) {
        for (size_t i = 0; i < SHM_SEGS; i++) {
            struct shm_seg* seg = &g_area->segs[i];
            if (!seg->used || seg->removed || seg->key != key)
                continue;
            long r;
            if ((shmflg & (LINUX_IPC_CREAT | LINUX_IPC_EXCL)) == (LINUX_IPC_CREAT | LINUX_IPC_EXCL))
                r = -EEXIST;
            else if (size && size > seg->size)
                r = -EINVAL;
            else if (!ipc_permitted(seg->mode, seg->uid, seg->gid, seg->cuid, seg->cgid,
                                    (unsigned int)shmflg & 0777))
                r = -EACCES; /* the kernel's ipcperms() with the requested mode bits */
            else
                r = (long)i;
            area_unlock();
            return r;
        }
        if (!(shmflg & LINUX_IPC_CREAT)) {
            area_unlock();
            return -ENOENT;
        }
    }
    if (!size) {
        area_unlock();
        return -EINVAL;
    }
    size_t index = SHM_SEGS;
    for (size_t i = 0; i < SHM_SEGS; i++) {
        if (!g_area->segs[i].used) {
            index = i;
            break;
        }
    }
    if (index == SHM_SEGS) {
        area_unlock();
        return -ENOSPC;
    }
    struct shm_seg* seg = &g_area->segs[index];
    /* reserve the slot, then create the file without the lock (file system calls may sleep) */
    *seg = (struct shm_seg){.used = true, .removed = true};
    area_unlock();

    char path[sizeof(seg->path)];
    ret = shm_create_file(size, path, sizeof(path));

    area_lock();
    if (ret < 0) {
        *seg = (struct shm_seg){0};
        area_unlock();
        return ret;
    }
    *seg = (struct shm_seg){
        .used  = true,
        .key   = key == LINUX_IPC_PRIVATE ? 0 : key,
        .seq   = (unsigned short)g_area->next_file,
        .size  = size,
        .mode  = (unsigned short)(shmflg & 0777),
        .uid   = (unsigned int)libos_syscall_getuid(),
        .gid   = (unsigned int)libos_syscall_getgid(),
        .cuid  = (unsigned int)libos_syscall_geteuid(),
        .cgid  = (unsigned int)libos_syscall_getegid(),
        .cpid  = g_process.pid,
        .lpid  = g_process.pid,
        .ctime = now_seconds(),
    };
    memcpy(seg->path, path, sizeof(seg->path));
    area_unlock();
    return (long)index;
}

long sysv_shared_shmat(int shmid, const void* shmaddr, int shmflg) {
    if (shmflg & ~(LINUX_SHM_RDONLY | LINUX_SHM_RND | LINUX_SHM_REMAP | LINUX_SHM_EXEC))
        return -EINVAL;
    int ret = area_get();
    if (ret < 0)
        return ret;

    area_lock();
    struct shm_seg* seg = shm_by_id(shmid);
    if (!seg) {
        area_unlock();
        return -EINVAL;
    }
    unsigned short need = (shmflg & LINUX_SHM_RDONLY) ? 04 : 06;
    if ((shm_class_bits(seg) & need) != need) {
        area_unlock();
        return -EACCES;
    }
    uintptr_t addr = (uintptr_t)shmaddr;
    if (addr) {
        if (shmflg & LINUX_SHM_RND)
            addr &= ~(uintptr_t)(LINUX_SHMLBA - 1);
        if (!(shmflg & (LINUX_SHM_RND | LINUX_SHM_REMAP)) && (addr % SHM_PAGE_SIZE)) {
            area_unlock();
            return -EINVAL;
        }
    }
    size_t size = seg->size;
    char path[sizeof(seg->path)];
    memcpy(path, seg->path, sizeof(path));
    /* the attach counts from now on, so that IPC_RMID meanwhile keeps the file */
    seg->nattach++;
    area_unlock();

    int prot = PROT_READ | ((shmflg & LINUX_SHM_RDONLY) ? 0 : PROT_WRITE)
               | ((shmflg & LINUX_SHM_EXEC) ? PROT_EXEC : 0);
    int flags = MAP_SHARED | ((shmflg & LINUX_SHM_REMAP) ? MAP_FIXED : 0);
    long fd = libos_openat_internal(AT_FDCWD, path,
                                    (shmflg & LINUX_SHM_RDONLY) ? O_RDONLY : O_RDWR, 0);
    void* mapped = (void*)fd;
    if (fd >= 0) {
        mapped = libos_syscall_mmap((void*)addr, size, prot, flags, (int)fd, 0);
        libos_syscall_close((int)fd);
    }

    char unlink_path[sizeof(seg->path)];
    bool do_unlink = false;
    area_lock();
    struct shm_attach* attach = NULL;
    size_t own = 0;
    for (size_t i = 0; i < SHM_ATTACH_ALL; i++) {
        struct shm_attach* a = &g_area->attach[i];
        if (a->pid == g_process.pid)
            own++;
        else if (!a->pid && !attach)
            attach = a;
    }
    bool map_failed = (long)mapped < 0 && (long)mapped >= -4095;
    /* no room to remember the range: unmap rather than leak a mapping shmdt() could not find */
    bool no_room = !map_failed && (!attach || own >= SHM_ATTACH);
    long result;
    if (map_failed || no_room) {
        result = map_failed ? (long)mapped : -ENOMEM;
        g_area->segs[shmid].nattach--;
        do_unlink = shm_maybe_destroy(&g_area->segs[shmid], unlink_path, sizeof(unlink_path));
    } else {
        result = (long)mapped;
        *attach = (struct shm_attach){.pid = g_process.pid, .addr = (uintptr_t)mapped, .len = size,
                                      .shmid = shmid};
        g_area->segs[shmid].atime = now_seconds();
        g_area->segs[shmid].lpid = g_process.pid;
    }
    area_unlock();
    if (no_room)
        libos_syscall_munmap(mapped, size);
    if (do_unlink)
        libos_unlink_internal(unlink_path);
    return result;
}

/* Drops the attach record `a` of this process; returns whether a file must be unlinked. */
static bool shm_detach_locked(struct shm_attach* a, char* path, size_t path_size) {
    struct shm_seg* seg = (a->shmid >= 0 && a->shmid < SHM_SEGS) ? &g_area->segs[a->shmid] : NULL;
    *a = (struct shm_attach){0};
    if (!seg || !seg->used)
        return false;
    assert(seg->nattach);
    seg->nattach--;
    seg->dtime = now_seconds();
    seg->lpid = g_process.pid;
    return shm_maybe_destroy(seg, path, path_size);
}

long sysv_shared_shmdt(const void* shmaddr) {
    uintptr_t addr = (uintptr_t)shmaddr;
    if (!addr || (addr % SHM_PAGE_SIZE))
        return -EINVAL;
    int ret = area_get();
    if (ret < 0)
        return ret;

    area_lock();
    struct shm_attach* attach = NULL;
    for (size_t i = 0; i < SHM_ATTACH_ALL; i++) {
        if (g_area->attach[i].pid == g_process.pid && g_area->attach[i].addr == addr) {
            attach = &g_area->attach[i];
            break;
        }
    }
    if (!attach) {
        area_unlock();
        return -EINVAL;
    }
    size_t len = attach->len;
    char path[sizeof(((struct shm_seg*)0)->path)];
    bool do_unlink = shm_detach_locked(attach, path, sizeof(path));
    area_unlock();

    long r = libos_syscall_munmap((void*)addr, len);
    if (do_unlink)
        libos_unlink_internal(path);
    return r < 0 ? r : 0;
}

static void shm_fill_ds(const struct shm_seg* seg, struct linux_shmid_ds* ds) {
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

long sysv_shared_shmctl(int shmid, int cmd, void* buf) {
    switch (cmd) {
        case LINUX_IPC_INFO: {
            if (!buf || !is_user_memory_writable(buf, sizeof(struct linux_shminfo64)))
                return -EFAULT;
            *(struct linux_shminfo64*)buf = (struct linux_shminfo64){
                .shmmax = SHM_SHMMAX,
                .shmin  = 1,
                .shmmni = SHM_SEGS,
                .shmseg = SHM_ATTACH,
                .shmall = SHM_SHMMAX,
            };
            return 0;
        }
        case LINUX_SHM_INFO:
            if (!buf || !is_user_memory_writable(buf, sizeof(struct linux_shm_info)))
                return -EFAULT;
            break;
        case LINUX_IPC_STAT:
        case LINUX_SHM_STAT:
        case LINUX_SHM_STAT_ANY:
            if (!buf || !is_user_memory_writable(buf, sizeof(struct linux_shmid_ds)))
                return -EFAULT;
            break;
        case LINUX_IPC_SET:
            if (!buf || !is_user_memory_readable(buf, sizeof(struct linux_shmid_ds)))
                return -EFAULT;
            break;
        case LINUX_IPC_RMID:
        case LINUX_SHM_LOCK:
        case LINUX_SHM_UNLOCK:
            break;
        default:
            return -EINVAL;
    }
    int ret = area_get();
    if (ret < 0)
        return ret;

    area_lock();
    if (cmd == LINUX_SHM_INFO) {
        struct linux_shm_info info = {0};
        long r = -EINVAL;
        size_t nth = 0;
        for (size_t i = 0; i < SHM_SEGS; i++) {
            struct shm_seg* seg = &g_area->segs[i];
            if (!seg->used || seg->removed)
                continue;
            info.used_ids++;
            info.shm_tot += (seg->size + SHM_PAGE_SIZE - 1) / SHM_PAGE_SIZE;
            if (nth++ == (size_t)shmid)
                r = seg->key;
        }
        area_unlock();
        if (r != -EINVAL)
            *(struct linux_shm_info*)buf = info;
        return r;
    }

    struct shm_seg* seg = NULL;
    size_t index = 0;
    if (cmd == LINUX_SHM_STAT || cmd == LINUX_SHM_STAT_ANY) {
        size_t nth = 0;
        for (size_t i = 0; shmid >= 0 && i < SHM_SEGS; i++) {
            if (!g_area->segs[i].used || g_area->segs[i].removed)
                continue;
            if (nth++ == (size_t)shmid) {
                seg = &g_area->segs[i];
                index = i;
                break;
            }
        }
    } else {
        seg = shm_by_id(shmid);
        index = (size_t)shmid;
    }
    if (!seg) {
        area_unlock();
        return -EINVAL;
    }

    long r = 0;
    char path[sizeof(seg->path)];
    bool do_unlink = false;
    unsigned int euid = (unsigned int)libos_syscall_geteuid();
    switch (cmd) {
        case LINUX_IPC_STAT:
        case LINUX_SHM_STAT:
            if ((shm_class_bits(seg) & 04) != 04) {
                r = -EACCES;
                break;
            }
            /* fall through */
        case LINUX_SHM_STAT_ANY: {
            struct linux_shmid_ds ds;
            shm_fill_ds(seg, &ds);
            r = cmd == LINUX_IPC_STAT ? 0 : (long)index;
            *(struct linux_shmid_ds*)buf = ds;
            break;
        }
        case LINUX_IPC_SET: {
            struct linux_shmid_ds* ds = buf;
            if (euid != 0 && euid != seg->cuid && euid != seg->uid) {
                r = -EPERM;
                break;
            }
            seg->uid = ds->shm_perm.uid;
            seg->gid = ds->shm_perm.gid;
            seg->mode = (unsigned short)(ds->shm_perm.mode & 0777);
            seg->ctime = now_seconds();
            seg->lpid = g_process.pid;
            break;
        }
        case LINUX_IPC_RMID:
            if (euid != 0 && euid != seg->cuid && euid != seg->uid) {
                r = -EPERM;
                break;
            }
            seg->removed = true;
            seg->lpid = g_process.pid;
            seg->ctime = now_seconds();
            do_unlink = shm_maybe_destroy(seg, path, sizeof(path));
            break;
        case LINUX_SHM_LOCK:
        case LINUX_SHM_UNLOCK:
            if (euid != 0) {
                r = -EPERM;
                break;
            }
            seg->locked = cmd == LINUX_SHM_LOCK;
            seg->lpid = g_process.pid;
            break;
    }
    area_unlock();
    if (do_unlink)
        libos_unlink_internal(path);
    return r;
}

int sysv_shared_shm_proc_load(char** out_data, size_t* out_size) {
    static const char header[] =
        "key      shmid perms                  size  cpid  lpid nattch   uid   gid  cuid  cgid"
        "      atime      dtime      ctime                   rss                  swap\n";
    int ret = area_get();
    if (ret < 0)
        return ret;
    size_t cap = sizeof(header) + SHM_SEGS * 160;
    char* buf = malloc(cap);
    if (!buf)
        return -ENOMEM;
    size_t off = sizeof(header) - 1;
    memcpy(buf, header, off);
    area_lock();
    for (size_t i = 0; i < SHM_SEGS; i++) {
        struct shm_seg* seg = &g_area->segs[i];
        if (!seg->used || seg->removed)
            continue;
        int n = snprintf(buf + off, cap - off,
                         "%10d %10zu %5o %20zu %5d %5d %4zu %5u %5u %5u %5u %10ld %10ld %10ld"
                         " %16lu %16lu\n",
                         seg->key, i, seg->mode, seg->size, seg->cpid, seg->lpid, seg->nattach,
                         seg->uid, seg->gid, seg->cuid, seg->cgid, seg->atime, seg->dtime,
                         seg->ctime, 0UL, 0UL);
        off += (size_t)n;
    }
    area_unlock();
    *out_data = buf;
    *out_size = off;
    return 0;
}

/*
 * Message queues
 */

static int msq_id(const struct msq* q, size_t index) {
    return (int)(((unsigned int)q->seq << MSG_IDX_BITS) | index);
}

static struct msq* msq_lookup(int msqid, size_t* out_index) {
    if (msqid < 0)
        return NULL;
    size_t index = (unsigned int)msqid & ((1U << MSG_IDX_BITS) - 1);
    unsigned int seq = (unsigned int)msqid >> MSG_IDX_BITS;
    if (index >= MSG_MNI || !g_area->msqs[index].used || g_area->msqs[index].seq != seq)
        return NULL;
    *out_index = index;
    return &g_area->msqs[index];
}

static bool msq_permitted(const struct msq* q, unsigned int flag) {
    return ipc_permitted(q->mode, q->uid, q->gid, q->cuid, q->cgid, flag);
}

static void msq_free(size_t index) {
    struct msq* q = &g_area->msqs[index];
    heap_off_t off = q->head;
    while (off) {
        struct msg_hdr* msg = heap_ptr(off);
        heap_off_t next = msg->next;
        heap_free(off);
        off = next;
    }
    uint32_t wseq = q->wseq;
    *q = (struct msq){0};
    q->wseq = wseq;
    g_area->msg_in_use--;
    /* sleepers find their identifier gone and answer EIDRM */
    object_changed(&q->wseq);
}

long sysv_shared_msgget(int key, int msgflg) {
    int ret = area_get();
    if (ret < 0)
        return ret;
    area_lock();
    if (key != LINUX_IPC_PRIVATE) {
        for (size_t i = 0; i < MSG_MNI; i++) {
            struct msq* q = &g_area->msqs[i];
            if (!q->used || q->key != key)
                continue;
            long r;
            if ((msgflg & (LINUX_IPC_CREAT | LINUX_IPC_EXCL)) == (LINUX_IPC_CREAT | LINUX_IPC_EXCL))
                r = -EEXIST;
            else if (!msq_permitted(q, (unsigned int)msgflg & 0777))
                r = -EACCES;
            else
                r = msq_id(q, i);
            area_unlock();
            return r;
        }
        if (!(msgflg & LINUX_IPC_CREAT)) {
            area_unlock();
            return -ENOENT;
        }
    }
    size_t index = MSG_MNI;
    for (size_t i = 0; g_area->msg_in_use < MSG_MNI && i < MSG_MNI; i++) {
        if (!g_area->msqs[i].used) {
            index = i;
            break;
        }
    }
    if (index == MSG_MNI) {
        area_unlock();
        return -ENOSPC;
    }
    unsigned int euid = (unsigned int)libos_syscall_geteuid();
    unsigned int egid = (unsigned int)libos_syscall_getegid();
    struct msq* q = &g_area->msqs[index];
    uint32_t wseq = q->wseq;
    *q = (struct msq){
        .used   = true,
        .key    = key,
        .seq    = g_area->msg_next_seq[index]++,
        .mode   = (unsigned short)(msgflg & 0777),
        .uid    = euid,
        .gid    = egid,
        .cuid   = euid,
        .cgid   = egid,
        .ctime  = now_seconds(),
        .qbytes = MSG_MNB,
        .wseq   = wseq,
    };
    g_area->msg_in_use++;
    long r = msq_id(q, index);
    area_unlock();
    return r;
}

long sysv_shared_msgsnd(int msqid, const void* msgp, size_t msgsz, int msgflg) {
    if (!is_user_memory_readable(msgp, sizeof(long)))
        return -EFAULT;
    long mtype = *(const long*)msgp;
    if (msgsz > MSG_MAX || (long)msgsz < 0 || msqid < 0)
        return -EINVAL;
    if (mtype < 1)
        return -EINVAL;
    const char* mtext = (const char*)msgp + sizeof(long);
    if (msgsz && !is_user_memory_readable(mtext, msgsz))
        return -EFAULT;
    /* a copy first: the user buffer must not be read with the area locked (it may fault) */
    char* text = malloc(msgsz ?: 1);
    if (!text)
        return -ENOMEM;
    memcpy(text, mtext, msgsz);
    int ret = area_get();
    if (ret < 0) {
        free(text);
        return ret;
    }

    bool slept = false;
    long r;
    for (;;) {
        area_lock();
        size_t index;
        struct msq* q = msq_lookup(msqid, &index);
        if (!q) {
            r = slept ? -EIDRM : -EINVAL;
            break;
        }
        if (!msq_permitted(q, 0222)) {
            r = -EACCES;
            break;
        }
        heap_off_t off = 0;
        if (msgsz + q->cbytes <= q->qbytes && 1 + q->qnum <= q->qbytes)
            off = heap_alloc(sizeof(struct msg_hdr) + msgsz);
        if (off) {
            struct msg_hdr* msg = heap_ptr(off);
            msg->next = 0;
            msg->type = mtype;
            msg->len = msgsz;
            memcpy(msg->text, text, msgsz);
            if (q->tail)
                ((struct msg_hdr*)heap_ptr(q->tail))->next = off;
            else
                q->head = off;
            q->tail = off;
            q->cbytes += msgsz;
            q->qnum++;
            q->lspid = g_process.pid;
            q->stime = now_seconds();
            object_changed(&q->wseq); /* receivers waiting for a message */
            r = 0;
            break;
        }
        if (msgflg & LINUX_IPC_NOWAIT) {
            r = -EAGAIN;
            break;
        }
        uint32_t seen = __atomic_load_n(&q->wseq, __ATOMIC_ACQUIRE);
        int w = area_unlock_and_wait(&q->wseq, seen, /*timeout_us=*/NULL);
        slept = true;
        if (w == -EINTR) {
            free(text);
            return -ERESTARTNOHAND;
        }
    }
    area_unlock();
    free(text);
    return r;
}

enum msg_search { SEARCH_ANY, SEARCH_EQUAL, SEARCH_NOTEQUAL, SEARCH_LESSEQUAL, SEARCH_NUMBER };

static heap_off_t msq_find(struct msq* q, long type, enum msg_search mode, heap_off_t* out_prev) {
    heap_off_t found = 0, found_prev = 0, prev = 0;
    long count = 0;
    for (heap_off_t off = q->head; off; prev = off, off = ((struct msg_hdr*)heap_ptr(off))->next) {
        struct msg_hdr* msg = heap_ptr(off);
        bool match;
        switch (mode) {
            case SEARCH_ANY:       match = true; break;
            case SEARCH_EQUAL:     match = msg->type == type; break;
            case SEARCH_NOTEQUAL:  match = msg->type != type; break;
            case SEARCH_LESSEQUAL: match = msg->type <= type; break;
            case SEARCH_NUMBER:
            default:               match = count++ == type; break;
        }
        if (!match)
            continue;
        if (mode == SEARCH_LESSEQUAL && msg->type != 1) {
            type = msg->type - 1;
            found = off;
            found_prev = prev;
            continue;
        }
        found = off;
        found_prev = prev;
        break;
    }
    *out_prev = found_prev;
    return found;
}

long sysv_shared_msgrcv(int msqid, void* msgp, size_t msgsz, long msgtyp, int msgflg) {
    if (msqid < 0 || (long)msgsz < 0)
        return -EINVAL;
    if (msgflg & LINUX_MSG_COPY) {
        if ((msgflg & LINUX_MSG_EXCEPT) || !(msgflg & LINUX_IPC_NOWAIT))
            return -EINVAL;
        size_t copy_size = MIN(msgsz, (size_t)MSG_MAX);
        if (!is_user_memory_readable(msgp, sizeof(long) + copy_size))
            return -EFAULT;
    }
    enum msg_search mode;
    if (msgflg & LINUX_MSG_COPY) {
        mode = SEARCH_NUMBER;
    } else if (msgtyp == 0) {
        mode = SEARCH_ANY;
    } else if (msgtyp < 0) {
        mode = SEARCH_LESSEQUAL;
        msgtyp = msgtyp == LONG_MIN ? LONG_MAX : -msgtyp;
    } else if (msgflg & LINUX_MSG_EXCEPT) {
        mode = SEARCH_NOTEQUAL;
    } else {
        mode = SEARCH_EQUAL;
    }
    int ret = area_get();
    if (ret < 0)
        return ret;

    /* the message is copied out of the area into `copy` under the lock */
    char* copy = malloc(MSG_MAX);
    if (!copy)
        return -ENOMEM;
    long copy_type = 0;
    size_t copy_len = 0;
    bool slept = false;
    long r;
    for (;;) {
        area_lock();
        size_t index;
        struct msq* q = msq_lookup(msqid, &index);
        if (!q) {
            r = slept ? -EIDRM : -EINVAL;
            break;
        }
        if (!msq_permitted(q, 0444)) {
            r = -EACCES;
            break;
        }
        heap_off_t prev;
        heap_off_t found_off = msq_find(q, msgtyp, mode, &prev);
        if (found_off) {
            struct msg_hdr* found = heap_ptr(found_off);
            if (msgsz < found->len && !(msgflg & LINUX_MSG_NOERROR)) {
                r = -E2BIG;
                break;
            }
            if ((msgflg & LINUX_MSG_COPY) && MIN(msgsz, (size_t)MSG_MAX) < found->len) {
                r = -EINVAL;
                break;
            }
            copy_type = found->type;
            copy_len = found->len;
            memcpy(copy, found->text, found->len);
            if (!(msgflg & LINUX_MSG_COPY)) {
                if (prev)
                    ((struct msg_hdr*)heap_ptr(prev))->next = found->next;
                else
                    q->head = found->next;
                if (q->tail == found_off)
                    q->tail = prev;
                q->cbytes -= found->len;
                q->qnum--;
                q->lrpid = g_process.pid;
                q->rtime = now_seconds();
                heap_free(found_off);
                object_changed(&q->wseq); /* senders waiting for room */
            }
            r = 1;
            break;
        }
        if (msgflg & LINUX_IPC_NOWAIT) {
            r = -ENOMSG;
            break;
        }
        uint32_t seen = __atomic_load_n(&q->wseq, __ATOMIC_ACQUIRE);
        int w = area_unlock_and_wait(&q->wseq, seen, /*timeout_us=*/NULL);
        slept = true;
        if (w == -EINTR) {
            free(copy);
            return -ERESTARTNOHAND;
        }
    }
    area_unlock();
    if (r == 1) {
        /* as in the kernel, a message that cannot be stored is lost (it left the queue) */
        size_t n = MIN(msgsz, copy_len);
        if (!is_user_memory_writable(msgp, sizeof(long) + n)) {
            r = -EFAULT;
        } else {
            *(long*)msgp = copy_type;
            memcpy((char*)msgp + sizeof(long), copy, n);
            r = (long)n;
        }
    }
    free(copy);
    return r;
}

static void msq_fill_ds(const struct msq* q, struct linux_msqid_ds* ds) {
    *ds = (struct linux_msqid_ds){
        .msg_perm = {
            .key  = q->key,
            .uid  = q->uid,
            .gid  = q->gid,
            .cuid = q->cuid,
            .cgid = q->cgid,
            .mode = q->mode,
            .seq  = q->seq,
        },
        .msg_stime  = q->stime,
        .msg_rtime  = q->rtime,
        .msg_ctime  = q->ctime,
        .msg_cbytes = q->cbytes,
        .msg_qnum   = q->qnum,
        .msg_qbytes = q->qbytes,
        .msg_lspid  = q->lspid,
        .msg_lrpid  = q->lrpid,
    };
}

long sysv_shared_msgctl(int msqid, int cmd, void* buf) {
    if (msqid < 0 || cmd < 0)
        return -EINVAL;
    struct linux_msqid_ds set_ds;
    if (cmd == LINUX_IPC_SET) {
        if (!is_user_memory_readable(buf, sizeof(set_ds)))
            return -EFAULT;
        set_ds = *(struct linux_msqid_ds*)buf;
    } else if (cmd != LINUX_IPC_INFO && cmd != LINUX_MSG_INFO && cmd != LINUX_IPC_STAT &&
               cmd != LINUX_MSG_STAT && cmd != LINUX_MSG_STAT_ANY && cmd != LINUX_IPC_RMID) {
        return -EINVAL;
    }
    int ret = area_get();
    if (ret < 0)
        return ret;

    switch (cmd) {
        case LINUX_IPC_INFO:
        case LINUX_MSG_INFO: {
            struct linux_msginfo info = {
                .msgmni = MSG_MNI,
                .msgmax = MSG_MAX,
                .msgmnb = MSG_MNB,
                .msgssz = 16,
                .msgseg = 0xffff,
            };
            area_lock();
            long max_index = 0;
            size_t messages = 0, bytes = 0;
            for (size_t i = 0; i < MSG_MNI; i++) {
                if (!g_area->msqs[i].used)
                    continue;
                max_index = (long)i;
                messages += g_area->msqs[i].qnum;
                bytes += g_area->msqs[i].cbytes;
            }
            if (cmd == LINUX_MSG_INFO) {
                info.msgpool = (int)g_area->msg_in_use;
                info.msgmap  = (int)messages;
                info.msgtql  = (int)bytes;
            } else {
                info.msgpool = (MSG_MNI * MSG_MNB) / 1024;
                info.msgmap  = MSG_MNB;
                info.msgtql  = MSG_MNB;
            }
            area_unlock();
            if (!is_user_memory_writable(buf, sizeof(info)))
                return -EFAULT;
            *(struct linux_msginfo*)buf = info;
            return max_index;
        }
        case LINUX_IPC_STAT:
        case LINUX_MSG_STAT:
        case LINUX_MSG_STAT_ANY: {
            struct linux_msqid_ds ds;
            long r;
            area_lock();
            struct msq* q = NULL;
            size_t index = 0;
            if (cmd == LINUX_IPC_STAT) {
                q = msq_lookup(msqid, &index);
            } else if ((size_t)msqid < MSG_MNI && g_area->msqs[msqid].used) {
                index = (size_t)msqid;
                q = &g_area->msqs[index];
            }
            if (!q)
                r = -EINVAL;
            else if (cmd != LINUX_MSG_STAT_ANY && !msq_permitted(q, 0444))
                r = -EACCES;
            else {
                msq_fill_ds(q, &ds);
                r = cmd == LINUX_IPC_STAT ? 0 : msq_id(q, index);
            }
            area_unlock();
            if (r < 0)
                return r;
            if (!is_user_memory_writable(buf, sizeof(ds)))
                return -EFAULT;
            *(struct linux_msqid_ds*)buf = ds;
            return r;
        }
        case LINUX_IPC_SET: {
            area_lock();
            size_t index;
            struct msq* q = msq_lookup(msqid, &index);
            long r = 0;
            if (!q) {
                r = -EINVAL;
            } else if (!ipc_is_owner(q->uid, q->cuid)) {
                r = -EPERM;
            } else if (set_ds.msg_qbytes > MSG_MNB && !is_root()) {
                r = -EPERM;
            } else {
                q->qbytes = set_ds.msg_qbytes;
                q->uid = set_ds.msg_perm.uid;
                q->gid = set_ds.msg_perm.gid;
                q->mode = (unsigned short)(set_ds.msg_perm.mode & 0777);
                q->ctime = now_seconds();
                object_changed(&q->wseq);
            }
            area_unlock();
            return r;
        }
        case LINUX_IPC_RMID:
        default: {
            area_lock();
            size_t index;
            struct msq* q = msq_lookup(msqid, &index);
            long r = 0;
            if (!q)
                r = -EINVAL;
            else if (!ipc_is_owner(q->uid, q->cuid))
                r = -EPERM;
            else
                msq_free(index);
            area_unlock();
            return r;
        }
    }
}

int sysv_shared_msg_proc_load(char** out_data, size_t* out_size) {
    static const char header[] =
        "       key      msqid perms      cbytes       qnum lspid lrpid   uid   gid  cuid  cgid"
        "      stime      rtime      ctime\n";
    int ret = area_get();
    if (ret < 0)
        return ret;
    size_t cap = sizeof(header) + MSG_MNI * 160;
    char* buf = malloc(cap);
    if (!buf)
        return -ENOMEM;
    size_t off = sizeof(header) - 1;
    memcpy(buf, header, off);
    area_lock();
    for (size_t i = 0; i < MSG_MNI; i++) {
        struct msq* q = &g_area->msqs[i];
        if (!q->used)
            continue;
        int n = snprintf(buf + off, cap - off,
                         "%10d %10d  %4o  %10zu %10zu %5d %5d %5u %5u %5u %5u %10ld %10ld %10ld\n",
                         q->key, msq_id(q, i), q->mode, q->cbytes, q->qnum, q->lspid, q->lrpid,
                         q->uid, q->gid, q->cuid, q->cgid, q->stime, q->rtime, q->ctime);
        off += (size_t)n;
    }
    area_unlock();
    *out_data = buf;
    *out_size = off;
    return 0;
}

/*
 * Semaphores
 */

static int sem_id(const struct sem_set* set, size_t index) {
    return (int)(((unsigned int)set->seq << SEM_IDX_BITS) | index);
}

static struct sem_set* sem_lookup(int semid, size_t* out_index) {
    if (semid < 0)
        return NULL;
    size_t index = (unsigned int)semid & ((1U << SEM_IDX_BITS) - 1);
    unsigned int seq = (unsigned int)semid >> SEM_IDX_BITS;
    if (index >= SEM_MNI || !g_area->sets[index].used || g_area->sets[index].seq != seq)
        return NULL;
    *out_index = index;
    return &g_area->sets[index];
}

static struct sem_value* sem_values(const struct sem_set* set) {
    return heap_ptr(set->sems);
}

static bool sem_permitted(const struct sem_set* set, unsigned int flag) {
    return ipc_permitted(set->mode, set->uid, set->gid, set->cuid, set->cgid, flag);
}

long sysv_shared_semget(int key, int nsems, int semflg) {
    if (nsems < 0 || nsems > SEM_MSL)
        return -EINVAL;
    int ret = area_get();
    if (ret < 0)
        return ret;
    area_lock();
    if (key != LINUX_IPC_PRIVATE) {
        for (size_t i = 0; i < SEM_MNI; i++) {
            struct sem_set* set = &g_area->sets[i];
            if (!set->used || set->key != key)
                continue;
            long r;
            if ((semflg & (LINUX_IPC_CREAT | LINUX_IPC_EXCL)) == (LINUX_IPC_CREAT | LINUX_IPC_EXCL))
                r = -EEXIST;
            else if (nsems > set->nsems)
                r = -EINVAL;
            else if (!sem_permitted(set, (unsigned int)semflg & 0777))
                r = -EACCES;
            else
                r = sem_id(set, i);
            area_unlock();
            return r;
        }
        if (!(semflg & LINUX_IPC_CREAT)) {
            area_unlock();
            return -ENOENT;
        }
    }
    if (nsems == 0) {
        area_unlock();
        return -EINVAL;
    }
    size_t index = SEM_MNI;
    for (size_t i = 0; g_area->sem_in_use < SEM_MNI && i < SEM_MNI; i++) {
        if (!g_area->sets[i].used) {
            index = i;
            break;
        }
    }
    heap_off_t sems = index < SEM_MNI ? heap_alloc((size_t)nsems * sizeof(struct sem_value)) : 0;
    if (!sems) {
        area_unlock();
        return -ENOSPC;
    }
    memset(heap_ptr(sems), 0, (size_t)nsems * sizeof(struct sem_value));
    unsigned int euid = (unsigned int)libos_syscall_geteuid();
    unsigned int egid = (unsigned int)libos_syscall_getegid();
    struct sem_set* set = &g_area->sets[index];
    uint32_t wseq = set->wseq;
    *set = (struct sem_set){
        .used  = true,
        .key   = key,
        .seq   = g_area->sem_next_seq[index]++,
        .mode  = (unsigned short)(semflg & 0777),
        .uid   = euid, .cuid = euid,
        .gid   = egid, .cgid = egid,
        .ctime = now_seconds(),
        .nsems = nsems,
        .sems  = sems,
        .wseq  = wseq,
    };
    g_area->sem_in_use++;
    long r = sem_id(set, index);
    area_unlock();
    return r;
}

static void sem_forget_undo(size_t index, unsigned short seq) {
    for (size_t i = 0; i < SEM_UNDO_ALL; i++) {
        struct sem_undo* u = &g_area->undo[i];
        if (u->pid && u->index == index && u->seq == seq)
            *u = (struct sem_undo){0};
    }
}

static void sem_free(size_t index) {
    struct sem_set* set = &g_area->sets[index];
    heap_free(set->sems);
    sem_forget_undo(index, set->seq);
    uint32_t wseq = set->wseq;
    *set = (struct sem_set){0};
    set->wseq = wseq;
    g_area->sem_in_use--;
    object_changed(&set->wseq); /* sleepers find their set gone: EIDRM */
}

static void sem_fill_ds(const struct sem_set* set, struct shared_semid64_ds* ds) {
    memset(ds, 0, sizeof(*ds));
    ds->sem_perm.key = set->key;
    ds->sem_perm.uid = set->uid;
    ds->sem_perm.gid = set->gid;
    ds->sem_perm.cuid = set->cuid;
    ds->sem_perm.cgid = set->cgid;
    ds->sem_perm.mode = set->mode;
    ds->sem_perm.seq = set->seq;
    ds->sem_otime = set->otime;
    ds->sem_ctime = set->ctime;
    ds->sem_nsems = (unsigned long)set->nsems;
}

long sysv_shared_semctl(int semid, int semnum, int cmd, unsigned long arg) {
    if (semid < 0)
        return -EINVAL;
    int ret = area_get();
    if (ret < 0)
        return ret;

    switch (cmd) {
        case LINUX_IPC_INFO:
        case SEM_INFO: {
            struct shared_seminfo info = {
                .semmap = SEM_MNS, .semmni = SEM_MNI, .semmns = SEM_MNS, .semmnu = SEM_MNS,
                .semmsl = SEM_MSL, .semopm = SEM_OPM, .semume = SEM_OPM, .semusz = 20,
                .semvmx = SEM_VMX, .semaem = SEM_VMX,
            };
            area_lock();
            long max_index = 0;
            size_t sems = 0;
            for (size_t i = 0; i < SEM_MNI; i++) {
                if (g_area->sets[i].used) {
                    max_index = (long)i;
                    sems += (size_t)g_area->sets[i].nsems;
                }
            }
            if (cmd == SEM_INFO) {
                info.semusz = (int)g_area->sem_in_use;
                info.semaem = (int)sems;
            }
            area_unlock();
            if (!is_user_memory_writable((void*)arg, sizeof(info)))
                return -EFAULT;
            memcpy((void*)arg, &info, sizeof(info));
            return max_index;
        }
        case LINUX_IPC_STAT:
        case SEM_STAT:
        case SEM_STAT_ANY: {
            struct shared_semid64_ds ds;
            long r;
            area_lock();
            size_t index = 0;
            struct sem_set* set = NULL;
            if (cmd == LINUX_IPC_STAT) {
                set = sem_lookup(semid, &index);
            } else if ((size_t)semid < SEM_MNI && g_area->sets[semid].used) {
                index = (size_t)semid;
                set = &g_area->sets[index];
            }
            if (!set)
                r = -EINVAL;
            else if (cmd != SEM_STAT_ANY && !sem_permitted(set, 0444))
                r = -EACCES;
            else {
                sem_fill_ds(set, &ds);
                r = cmd == LINUX_IPC_STAT ? 0 : sem_id(set, index);
            }
            area_unlock();
            if (r < 0)
                return r;
            if (!is_user_memory_writable((void*)arg, sizeof(ds)))
                return -EFAULT;
            memcpy((void*)arg, &ds, sizeof(ds));
            return r;
        }
        default:
            break;
    }

    struct shared_semid64_ds new_ds;
    unsigned short* user_array = NULL;
    unsigned short* values = NULL;
    if (cmd == LINUX_IPC_SET) {
        if (!is_user_memory_readable((void*)arg, sizeof(new_ds)))
            return -EFAULT;
        memcpy(&new_ds, (void*)arg, sizeof(new_ds));
    } else if (cmd == SEM_GETALL || cmd == SEM_SETALL) {
        user_array = (unsigned short*)arg;
    } else if (cmd != LINUX_IPC_RMID && cmd != SEM_GETVAL && cmd != SEM_GETPID &&
               cmd != SEM_GETNCNT && cmd != SEM_GETZCNT && cmd != SEM_SETVAL) {
        return -EINVAL;
    }

    /* SETALL reads the caller's array before taking the area lock */
    int nsems_hint = 0;
    if (cmd == SEM_SETALL) {
        area_lock();
        size_t index;
        struct sem_set* set = sem_lookup(semid, &index);
        nsems_hint = set ? set->nsems : 0;
        area_unlock();
        if (nsems_hint) {
            values = malloc((size_t)nsems_hint * sizeof(*values));
            if (!values)
                return -ENOMEM;
            if (is_user_memory_readable(user_array, (size_t)nsems_hint * sizeof(*user_array)))
                memcpy(values, user_array, (size_t)nsems_hint * sizeof(*values));
            else {
                free(values);
                values = NULL;
            }
        }
    }

    area_lock();
    size_t index;
    struct sem_set* set = sem_lookup(semid, &index);
    long r = 0;
    int nsems = 0;
    if (!set) {
        r = -EINVAL;
        goto out;
    }
    nsems = set->nsems;
    struct sem_value* sems = sem_values(set);
    if ((cmd == SEM_GETVAL || cmd == SEM_GETPID || cmd == SEM_GETNCNT || cmd == SEM_GETZCNT ||
            cmd == SEM_SETVAL) && (semnum < 0 || semnum >= nsems)) {
        r = -EINVAL;
        goto out;
    }
    switch (cmd) {
        case LINUX_IPC_RMID:
            if (!ipc_is_owner(set->uid, set->cuid)) {
                r = -EPERM;
                break;
            }
            sem_free(index);
            break;
        case LINUX_IPC_SET:
            if (!ipc_is_owner(set->uid, set->cuid)) {
                r = -EPERM;
                break;
            }
            set->uid = new_ds.sem_perm.uid;
            set->gid = new_ds.sem_perm.gid;
            set->mode = (unsigned short)(new_ds.sem_perm.mode & 0777);
            set->ctime = now_seconds();
            break;
        case SEM_GETVAL:
        case SEM_GETPID:
        case SEM_GETNCNT:
        case SEM_GETZCNT:
            if (!sem_permitted(set, 0444)) {
                r = -EACCES;
                break;
            }
            if (cmd == SEM_GETVAL)
                r = sems[semnum].value;
            else if (cmd == SEM_GETPID)
                r = sems[semnum].pid;
            else if (cmd == SEM_GETNCNT)
                r = sems[semnum].ncnt;
            else
                r = sems[semnum].zcnt;
            break;
        case SEM_GETALL:
            if (!sem_permitted(set, 0444)) {
                r = -EACCES;
                break;
            }
            values = malloc((size_t)nsems * sizeof(*values));
            if (!values) {
                r = -ENOMEM;
                break;
            }
            for (int i = 0; i < nsems; i++)
                values[i] = sems[i].value;
            break;
        case SEM_SETVAL:
            if (!sem_permitted(set, 0222)) {
                r = -EACCES;
                break;
            }
            if ((int)arg < 0 || (int)arg > SEM_VMX) {
                r = -ERANGE;
                break;
            }
            sems[semnum].value = (unsigned short)(int)arg;
            sems[semnum].pid = g_process.pid;
            set->ctime = now_seconds();
            for (size_t i = 0; i < SEM_UNDO_ALL; i++) {
                struct sem_undo* u = &g_area->undo[i];
                if (u->pid && u->index == index && u->seq == set->seq && u->semnum == semnum)
                    *u = (struct sem_undo){0}; /* as the kernel, SETVAL clears the adjustments */
            }
            object_changed(&set->wseq);
            break;
        case SEM_SETALL:
            if (!sem_permitted(set, 0222)) {
                r = -EACCES;
                break;
            }
            if (!values || nsems != nsems_hint) {
                r = -EFAULT;
                break;
            }
            for (int i = 0; i < nsems; i++) {
                if (values[i] > SEM_VMX) {
                    r = -ERANGE;
                    break;
                }
            }
            if (r < 0)
                break;
            for (int i = 0; i < nsems; i++) {
                sems[i].value = values[i];
                sems[i].pid = g_process.pid;
            }
            sem_forget_undo(index, set->seq);
            set->ctime = now_seconds();
            object_changed(&set->wseq);
            break;
        default:
            r = -EINVAL;
            break;
    }
out:
    area_unlock();
    if (cmd == SEM_GETALL && values && r == 0) {
        if (!is_user_memory_writable(user_array, (size_t)nsems * sizeof(*user_array)))
            r = -EFAULT;
        else
            memcpy(user_array, values, (size_t)nsems * sizeof(*user_array));
    }
    free(values);
    return r;
}

/* Applies all operations atomically: 0 when applied, 1 when the caller must wait (`*out_block`
 * names the blocking operation), or a negative errno. */
static int sem_try_apply(struct sem_set* set, size_t index, const struct shared_sembuf* sops,
                         size_t nsops, const struct shared_sembuf** out_block) {
    struct sem_value* sems = sem_values(set);
    for (size_t i = 0; i < nsops; i++) {
        struct sem_value* sem = &sems[sops[i].sem_num];
        int value = sem->value;
        int op = sops[i].sem_op;
        if ((op == 0 && value != 0) || (op < 0 && value + op < 0)) {
            *out_block = &sops[i];
            for (size_t j = i; j-- > 0;)
                sems[sops[j].sem_num].value -= (unsigned short)sops[j].sem_op;
            return 1;
        }
        if (op > 0 && value + op > SEM_VMX) {
            for (size_t j = i; j-- > 0;)
                sems[sops[j].sem_num].value -= (unsigned short)sops[j].sem_op;
            return -ERANGE;
        }
        sem->value = (unsigned short)(value + op);
    }
    /* record the SEM_UNDO adjustments; without room, roll back as the kernel does on ENOMEM */
    for (size_t i = 0; i < nsops; i++) {
        if (!(sops[i].sem_flg & SEM_UNDO) || !sops[i].sem_op)
            continue;
        struct sem_undo* slot = NULL;
        for (size_t k = 0; k < SEM_UNDO_ALL; k++) {
            struct sem_undo* u = &g_area->undo[k];
            if (u->pid == g_process.pid && u->index == index && u->seq == set->seq &&
                    u->semnum == sops[i].sem_num) {
                slot = u;
                break;
            }
            if (!u->pid && !slot)
                slot = u;
        }
        if (!slot) {
            for (size_t j = nsops; j-- > 0;)
                sems[sops[j].sem_num].value -= (unsigned short)sops[j].sem_op;
            return -ENOMEM;
        }
        if (slot->pid != g_process.pid)
            *slot = (struct sem_undo){.pid = g_process.pid, .index = (unsigned int)index,
                                      .seq = set->seq, .semnum = sops[i].sem_num};
        slot->adj -= sops[i].sem_op;
    }
    for (size_t i = 0; i < nsops; i++)
        sems[sops[i].sem_num].pid = g_process.pid;
    set->otime = now_seconds();
    return 0;
}

long sysv_shared_semtimedop(int semid, const struct linux_sembuf* user_sops_opaque, size_t nsops,
                            const struct __kernel_timespec* timeout) {
    const struct shared_sembuf* user_sops = (const struct shared_sembuf*)user_sops_opaque;
    uint64_t timeout_us = 0;
    if (timeout) {
        if (!is_user_memory_readable(timeout, sizeof(*timeout)))
            return -EFAULT;
        if (timeout->tv_sec < 0 || timeout->tv_nsec < 0 ||
                (uint64_t)timeout->tv_nsec >= TIME_NS_IN_S)
            return -EINVAL;
        timeout_us = timespec_to_us(timeout);
    }
    if (semid < 0 || nsops < 1)
        return -EINVAL;
    if (nsops > SEM_OPM)
        return -E2BIG;
    if (!is_user_memory_readable(user_sops, nsops * sizeof(*user_sops)))
        return -EFAULT;
    int ret = area_get();
    if (ret < 0)
        return ret;

    struct shared_sembuf* sops = malloc(nsops * sizeof(*sops));
    if (!sops)
        return -ENOMEM;
    memcpy(sops, user_sops, nsops * sizeof(*sops));
    bool alter = false;
    unsigned int max_num = 0;
    for (size_t i = 0; i < nsops; i++) {
        max_num = MAX(max_num, sops[i].sem_num);
        alter |= sops[i].sem_op != 0;
    }
    uint64_t deadline_us = 0;
    if (timeout) {
        uint64_t now = 0;
        if (PalSystemTimeQuery(&now) < 0) {
            free(sops);
            return -EINVAL;
        }
        deadline_us = now + timeout_us;
    }

    bool slept = false;
    long r;
    for (;;) {
        area_lock();
        size_t index;
        struct sem_set* set = sem_lookup(semid, &index);
        if (!set) {
            r = slept ? -EIDRM : -EINVAL;
            break;
        }
        if (max_num >= (unsigned int)set->nsems) {
            r = -EFBIG;
            break;
        }
        if (!sem_permitted(set, alter ? 0222 : 0444)) {
            r = -EACCES;
            break;
        }
        const struct shared_sembuf* block = NULL;
        int a = sem_try_apply(set, index, sops, nsops, &block);
        if (a <= 0) {
            r = a;
            if (a == 0)
                object_changed(&set->wseq);
            break;
        }
        if (block->sem_flg & LINUX_IPC_NOWAIT) {
            r = -EAGAIN;
            break;
        }
        uint64_t wait_us = 0;
        if (timeout) {
            uint64_t now = 0;
            if (PalSystemTimeQuery(&now) < 0 || now >= deadline_us) {
                r = -EAGAIN;
                break;
            }
            wait_us = deadline_us - now;
        }
        /* GETNCNT/GETZCNT count the sleepers; the counts live on the semaphore itself */
        struct sem_value* sem = &sem_values(set)[block->sem_num];
        bool zero = block->sem_op == 0;
        unsigned short seq = set->seq;
        unsigned short semnum = block->sem_num;
        if (zero)
            sem->zcnt++;
        else
            sem->ncnt++;
        uint32_t seen = __atomic_load_n(&set->wseq, __ATOMIC_ACQUIRE);
        int w = area_unlock_and_wait(&set->wseq, seen, timeout ? &wait_us : NULL);
        area_lock();
        if (g_area->sets[index].used && g_area->sets[index].seq == seq) {
            struct sem_value* s = &sem_values(&g_area->sets[index])[semnum];
            if (zero)
                s->zcnt--;
            else
                s->ncnt--;
        }
        area_unlock();
        slept = true;
        if (w == -EINTR) {
            free(sops);
            return -ERESTARTNOHAND;
        }
    }
    area_unlock();
    free(sops);
    return r;
}

/*
 * Process events
 */

void sysv_shared_fork(IDTYPE child_pid) {
    if (!g_enabled || !__atomic_load_n(&g_area, __ATOMIC_ACQUIRE))
        return; /* this process never used System V IPC: nothing to inherit */
    area_lock();
    for (size_t i = 0; i < SHM_ATTACH_ALL; i++) {
        struct shm_attach* a = &g_area->attach[i];
        if (a->pid != g_process.pid)
            continue;
        for (size_t k = 0; k < SHM_ATTACH_ALL; k++) {
            struct shm_attach* c = &g_area->attach[k];
            if (c->pid)
                continue;
            *c = *a;
            c->pid = child_pid;
            g_area->segs[a->shmid].nattach++;
            break;
        }
    }
    area_unlock();
}

/* Drops this process's attachments (their mappings go with the process or its old image). */
static void detach_all(void) {
    for (;;) {
        char path[sizeof(((struct shm_seg*)0)->path)];
        bool do_unlink = false;
        bool found = false;
        area_lock();
        for (size_t i = 0; i < SHM_ATTACH_ALL; i++) {
            if (g_area->attach[i].pid == g_process.pid) {
                do_unlink = shm_detach_locked(&g_area->attach[i], path, sizeof(path));
                found = true;
                if (do_unlink)
                    break;
            }
        }
        area_unlock();
        if (do_unlink)
            libos_unlink_internal(path);
        if (!found || !do_unlink)
            break;
    }
}

void sysv_shared_exec(void) {
    if (!g_enabled || !__atomic_load_n(&g_area, __ATOMIC_ACQUIRE))
        return;
    detach_all();
}

void sysv_shared_exit(void) {
    if (!g_enabled)
        return;
    if (!__atomic_load_n(&g_area, __ATOMIC_ACQUIRE)) {
        /* a forked child that inherited attachments but never called System V IPC itself */
        PAL_STREAM_ATTR attr;
        if (PalStreamAttributesQuery(SYSV_SHARED_URI, &attr) < 0 || area_get() < 0)
            return;
    }
    detach_all();
    /* SEM_UNDO: apply this process's adjustments (clamped at 0 and SEMVMX, as the kernel) */
    area_lock();
    for (size_t i = 0; i < SEM_UNDO_ALL; i++) {
        struct sem_undo* u = &g_area->undo[i];
        if (u->pid != g_process.pid)
            continue;
        struct sem_set* set = &g_area->sets[u->index];
        if (set->used && set->seq == u->seq && u->semnum < set->nsems) {
            struct sem_value* sem = &sem_values(set)[u->semnum];
            int value = (int)sem->value + u->adj;
            sem->value = (unsigned short)MIN(MAX(value, 0), SEM_VMX);
            sem->pid = g_process.pid;
            object_changed(&set->wseq);
        }
        *u = (struct sem_undo){0};
    }
    area_unlock();
}
