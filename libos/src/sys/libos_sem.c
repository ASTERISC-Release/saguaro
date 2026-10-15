/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/*
 * System V semaphores (semget/semctl/semop/semtimedop).
 *
 * A process-local table like the shared memory and message queues (`libos_sysv_shm.c`,
 * `libos_sysv_msg.c`): sets of up to SEMMSL semaphores, `seq << 15 | index` identifiers, the
 * kernel's `ipcperms()` checks, all semctl() commands, and semop() applying all operations
 * atomically or waiting (woken by any change of the set, by IPC_RMID with EIDRM, by a signal, or by
 * the semtimedop() timeout). SEM_UNDO is accepted but not recorded: the adjustments would be
 * applied at process exit, when nobody else can see this table anyway. A forked child (direct
 * PAL) starts with an empty table.
 *
 * With `sys.experimental__enable_host_sysv_semaphores` (VM/TDX with the graphics host agent) the
 * calls are forwarded to the host instead, one operation per semop().
 */

#include "libos_lock.h"
#include "libos_internal.h"
#include "libos_sysv_shared.h"
#include "libos_graphics.h"
#include "libos_process.h"
#include "libos_signal.h"
#include "libos_thread.h"
#include "libos_table.h"
#include "libos_utils.h"
#include "linux_abi/errors.h"
#include "pal.h"
#include "toml_utils.h"

#define IPC_CREAT 01000
#define IPC_EXCL 02000
#define IPC_NOWAIT 04000
#define SEM_UNDO 010000
#define IPC_RMID 0
#define IPC_SET 1
#define IPC_STAT 2
#define IPC_INFO 3
#define GETPID 11
#define GETVAL 12
#define GETALL 13
#define GETNCNT 14
#define GETZCNT 15
#define SETVAL 16
#define SETALL 17
#define SEM_STAT 18
#define SEM_INFO 19
#define SEM_STAT_ANY 20

/* Limits, published in /proc/sys/kernel/sem ("SEMMSL SEMMNS SEMOPM SEMMNI"). SEMMSL, SEMOPM and
 * SEMVMX are the kernel's; SEMMNI is smaller than its 32000 so the table stays small (LTP
 * semget05 creates sets until ENOSPC). */
#define SYSV_SEMMNI 1024
#define SYSV_SEMMSL 32000
#define SYSV_SEMOPM 500
#define SYSV_SEMVMX 32767
#define SYSV_SEMMNS (SYSV_SEMMNI * SYSV_SEMMSL)
#define SYSV_SEM_IDX_BITS 15

struct linux_sembuf {
    unsigned short sem_num;
    short sem_op;
    short sem_flg;
};

struct linux_ipc64_perm {
    int key;
    unsigned int uid, gid, cuid, cgid;
    unsigned short mode;
    unsigned char __pad1[4 - sizeof(unsigned short)];
    unsigned short seq;
    unsigned short __pad2;
    unsigned long __unused1, __unused2;
};

/* `struct semid64_ds`: x86_64 keeps the 32-bit layout's padding words (asm/sembuf.h), the
 * asm-generic 64-bit layout (aarch64, riscv64) does not */
struct linux_semid64_ds {
    struct linux_ipc64_perm sem_perm;
    long sem_otime;
#ifdef __x86_64__
    unsigned long __unused1;
#endif
    long sem_ctime;
#ifdef __x86_64__
    unsigned long __unused2;
#endif
    unsigned long sem_nsems;
    unsigned long __unused3, __unused4;
};

struct linux_seminfo {
    int semmap, semmni, semmns, semmnu, semmsl, semopm, semume, semusz, semvmx, semaem;
};

struct sysv_sem {
    unsigned short value;
    int pid; /* of the last successful semop/SETVAL/SETALL on this semaphore */
};

struct sysv_sem_set {
    int key;
    unsigned short seq;
    unsigned short mode;
    unsigned int uid, gid, cuid, cgid;
    long otime, ctime;
    int nsems;
    struct sysv_sem sems[];
};

/* A thread sleeping in semop(); lives on its stack while listed. `semnum`/`zero` describe the
 * operation it waits for (GETNCNT/GETZCNT count these). */
struct sysv_sem_waiter {
    struct sysv_sem_waiter* next;
    struct libos_thread* thread;
    size_t index;
    unsigned short semnum;
    bool zero;
};

static struct libos_lock g_sysv_sem_lock;
static struct sysv_sem_set* g_sem_sets[SYSV_SEMMNI];
static unsigned short g_sem_next_seq[SYSV_SEMMNI];
static size_t g_sem_in_use;
static struct sysv_sem_waiter* g_sem_waiters;
static bool g_use_host_sysv_semaphores;

#ifdef ENABLE_GRAPHICS
static long host_sem_request(uint64_t request, uint64_t p1, uint64_t p2, uint64_t p3,
                             uint64_t p4) {
    long offset = libos_syscall_acquire_libos_lock();
    comm_page_t* c = comm_page(offset);
    c->p1 = p1;
    c->p2 = p2;
    c->p3 = p3;
    c->p4 = p4;
    c->req_bit = request;
    long ret = (int64_t)comm_sync_notify(c);
    libos_syscall_relinquish_libos_lock(offset);
    return ret;
}
#else
/* The request ids come from graphics_comm_abi.h (through libos_graphics.h), which is included
 * whether or not graphics is built in; the call sites below are unreachable in that case. */
static long host_sem_request(uint64_t request, uint64_t p1, uint64_t p2, uint64_t p3,
                             uint64_t p4) {
    __UNUSED(request);
    __UNUSED(p1);
    __UNUSED(p2);
    __UNUSED(p3);
    __UNUSED(p4);
    return -ENOSYS; /* unreachable: init_sysv_semaphores() rejects the manifest option */
}
#endif

int init_sysv_semaphores(void) {
    int ret = toml_bool_in(g_manifest_root, "sys.experimental__enable_host_sysv_semaphores",
                           /*defaultval=*/false, &g_use_host_sysv_semaphores);
    if (ret < 0) {
        log_error("Cannot parse 'sys.experimental__enable_host_sysv_semaphores'");
        return -EINVAL;
    }
    /* Host semaphores are forwarded to the host agent through the graphics communication window, which
     * only exists in the VM/TDX deployment. Elsewhere the first semget() would fault on the
     * unmapped window, so refuse the option up front. */
#ifdef ENABLE_GRAPHICS
    bool host_agent = graphics_redirect_enabled();
#else
    bool host_agent = false;
#endif
    if (g_use_host_sysv_semaphores && !host_agent) {
        log_error("'sys.experimental__enable_host_sysv_semaphores' requires a host agent (VM/TDX "
                  "PAL with graphics enabled)");
        return -EINVAL;
    }
    return create_lock(&g_sysv_sem_lock) ? 0 : -ENOMEM;
}

static long sem_now_seconds(void) {
    uint64_t us = 0;
    if (PalSystemTimeQuery(&us) < 0)
        return 0;
    return (long)(us / 1000000ULL);
}

static int sem_id(const struct sysv_sem_set* set, size_t index) {
    return (int)(((unsigned int)set->seq << SYSV_SEM_IDX_BITS) | index);
}

static struct sysv_sem_set* sem_lookup(int semid, size_t* out_index) {
    assert(locked(&g_sysv_sem_lock));
    if (semid < 0)
        return NULL;
    size_t index = (unsigned int)semid & ((1U << SYSV_SEM_IDX_BITS) - 1);
    unsigned int seq = (unsigned int)semid >> SYSV_SEM_IDX_BITS;
    if (index >= SYSV_SEMMNI || !g_sem_sets[index] || g_sem_sets[index]->seq != seq)
        return NULL;
    *out_index = index;
    return g_sem_sets[index];
}

static bool sem_is_root(void) {
    return (unsigned int)libos_syscall_geteuid() == 0;
}

/* the kernel's `ipcperms()`, as for the message queues (`libos_sysv_msg.c`) */
static bool sem_permitted(const struct sysv_sem_set* set, unsigned int flag) {
    if (sem_is_root())
        return true;
    unsigned int euid = (unsigned int)libos_syscall_geteuid();
    unsigned int egid = (unsigned int)libos_syscall_getegid();
    unsigned int requested = ((flag >> 6) | (flag >> 3) | flag) & 07;
    unsigned int granted = set->mode;
    if (euid == set->cuid || euid == set->uid)
        granted >>= 6;
    else if (egid == set->cgid || egid == set->gid)
        granted >>= 3;
    return !(requested & ~granted & 07);
}

static bool sem_is_owner(const struct sysv_sem_set* set) {
    unsigned int euid = (unsigned int)libos_syscall_geteuid();
    return sem_is_root() || euid == set->cuid || euid == set->uid;
}

static void sem_wake_waiters(size_t index) {
    assert(locked(&g_sysv_sem_lock));
    for (struct sysv_sem_waiter* w = g_sem_waiters; w; w = w->next)
        if (w->index == index)
            thread_wakeup(w->thread);
}

static void sem_remove_waiter(struct sysv_sem_waiter* waiter) {
    assert(locked(&g_sysv_sem_lock));
    for (struct sysv_sem_waiter** p = &g_sem_waiters; *p; p = &(*p)->next) {
        if (*p == waiter) {
            *p = waiter->next;
            return;
        }
    }
}

long libos_syscall_semget(int key, int nsems, int semflg) {
    /* --- guest-wide System V IPC (VM, sys.experimental__multiprocess): libos_sysv_shared.c --- */
    if (sysv_shared_sem_enabled())
        return sysv_shared_semget(key, nsems, semflg);
    /* --- end of guest-wide System V IPC --- */
    if (g_use_host_sysv_semaphores)
        return host_sem_request(SYSV_SEMGET, key, nsems, semflg, 0);

    if (nsems < 0 || nsems > SYSV_SEMMSL)
        return -EINVAL;

    lock(&g_sysv_sem_lock);
    if (key != 0 /* IPC_PRIVATE */) {
        for (size_t i = 0; i < SYSV_SEMMNI; i++) {
            struct sysv_sem_set* set = g_sem_sets[i];
            if (!set || set->key != key)
                continue;
            long ret;
            if ((semflg & (IPC_CREAT | IPC_EXCL)) == (IPC_CREAT | IPC_EXCL))
                ret = -EEXIST;
            else if (nsems > set->nsems)
                ret = -EINVAL;
            else if (!sem_permitted(set, (unsigned int)semflg & 0777))
                ret = -EACCES;
            else
                ret = sem_id(set, i);
            unlock(&g_sysv_sem_lock);
            return ret;
        }
        if (!(semflg & IPC_CREAT)) {
            unlock(&g_sysv_sem_lock);
            return -ENOENT;
        }
    }
    if (nsems == 0) {
        unlock(&g_sysv_sem_lock);
        return -EINVAL;
    }

    size_t index = SYSV_SEMMNI;
    if (g_sem_in_use < SYSV_SEMMNI) {
        for (size_t i = 0; i < SYSV_SEMMNI; i++) {
            if (!g_sem_sets[i]) {
                index = i;
                break;
            }
        }
    }
    if (index == SYSV_SEMMNI) {
        unlock(&g_sysv_sem_lock);
        return -ENOSPC;
    }
    struct sysv_sem_set* set = calloc(1, sizeof(*set) + (size_t)nsems * sizeof(struct sysv_sem));
    if (!set) {
        unlock(&g_sysv_sem_lock);
        return -ENOMEM;
    }
    unsigned int euid = (unsigned int)libos_syscall_geteuid();
    unsigned int egid = (unsigned int)libos_syscall_getegid();
    set->key = key;
    set->seq = g_sem_next_seq[index]++;
    set->mode = (unsigned short)(semflg & 0777);
    set->uid = set->cuid = euid;
    set->gid = set->cgid = egid;
    set->ctime = sem_now_seconds();
    set->nsems = nsems;
    g_sem_sets[index] = set;
    g_sem_in_use++;
    long ret = sem_id(set, index);
    unlock(&g_sysv_sem_lock);
    return ret;
}

static void sem_fill_ds(const struct sysv_sem_set* set, struct linux_semid64_ds* ds) {
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

long libos_syscall_semctl(int semid, int semnum, int cmd, unsigned long arg) {
    /* --- guest-wide System V IPC (VM, sys.experimental__multiprocess): libos_sysv_shared.c --- */
    if (sysv_shared_sem_enabled())
        return sysv_shared_semctl(semid, semnum, cmd, arg);
    /* --- end of guest-wide System V IPC --- */
    if (g_use_host_sysv_semaphores)
        return host_sem_request(SYSV_SEMCTL, semid, semnum, cmd, arg);

    if (semid < 0)
        return -EINVAL;

    switch (cmd) {
        case IPC_INFO:
        case SEM_INFO: {
            struct linux_seminfo info = {
                .semmap = SYSV_SEMMNS,
                .semmni = SYSV_SEMMNI,
                .semmns = SYSV_SEMMNS,
                .semmnu = SYSV_SEMMNS,
                .semmsl = SYSV_SEMMSL,
                .semopm = SYSV_SEMOPM,
                .semume = SYSV_SEMOPM,
                .semusz = 20,
                .semvmx = SYSV_SEMVMX,
                .semaem = SYSV_SEMVMX,
            };
            lock(&g_sysv_sem_lock);
            long max_index = 0;
            size_t sems = 0;
            for (size_t i = 0; i < SYSV_SEMMNI; i++) {
                if (g_sem_sets[i]) {
                    max_index = (long)i;
                    sems += (size_t)g_sem_sets[i]->nsems;
                }
            }
            if (cmd == SEM_INFO) {
                /* the kernel reports the sets in use and the semaphores allocated here */
                info.semusz = (int)g_sem_in_use;
                info.semaem = (int)sems;
            }
            unlock(&g_sysv_sem_lock);
            void* buf = (void*)arg;
            if (!is_user_memory_writable(buf, sizeof(info)))
                return -EFAULT;
            memcpy(buf, &info, sizeof(info));
            return max_index;
        }
        case IPC_STAT:
        case SEM_STAT:
        case SEM_STAT_ANY: {
            struct linux_semid64_ds ds;
            long ret;
            lock(&g_sysv_sem_lock);
            size_t index = 0;
            struct sysv_sem_set* set = NULL;
            if (cmd == IPC_STAT) {
                set = sem_lookup(semid, &index);
            } else if ((size_t)semid < SYSV_SEMMNI) {
                index = (size_t)semid; /* SEM_STAT takes an index, not an identifier */
                set = g_sem_sets[index];
            }
            if (!set) {
                ret = -EINVAL;
            } else if (cmd != SEM_STAT_ANY && !sem_permitted(set, 0444)) {
                ret = -EACCES;
            } else {
                sem_fill_ds(set, &ds);
                ret = cmd == IPC_STAT ? 0 : sem_id(set, index);
            }
            unlock(&g_sysv_sem_lock);
            if (ret < 0)
                return ret;
            if (!is_user_memory_writable((void*)arg, sizeof(ds)))
                return -EFAULT;
            memcpy((void*)arg, &ds, sizeof(ds));
            return ret;
        }
        default:
            break;
    }

    /* the commands on one set, by identifier */
    struct linux_semid64_ds new_ds;
    unsigned short* user_array = NULL;
    unsigned short* values = NULL;
    if (cmd == IPC_SET) {
        if (!is_user_memory_readable((void*)arg, sizeof(new_ds)))
            return -EFAULT;
        memcpy(&new_ds, (void*)arg, sizeof(new_ds));
    } else if (cmd == GETALL || cmd == SETALL) {
        user_array = (unsigned short*)arg;
    } else if (cmd != IPC_RMID && cmd != GETVAL && cmd != GETPID && cmd != GETNCNT &&
               cmd != GETZCNT && cmd != SETVAL) {
        return -EINVAL;
    }

    lock(&g_sysv_sem_lock);
    size_t index;
    struct sysv_sem_set* set = sem_lookup(semid, &index);
    long ret = 0;
    if (!set) {
        ret = -EINVAL;
        goto out;
    }
    int nsems = set->nsems;
    if ((cmd == GETVAL || cmd == GETPID || cmd == GETNCNT || cmd == GETZCNT || cmd == SETVAL) &&
            (semnum < 0 || semnum >= nsems)) {
        ret = -EINVAL;
        goto out;
    }

    switch (cmd) {
        case IPC_RMID:
            if (!sem_is_owner(set)) {
                ret = -EPERM;
                break;
            }
            g_sem_sets[index] = NULL;
            g_sem_in_use--;
            free(set);
            sem_wake_waiters(index); /* they find their set gone: EIDRM */
            break;
        case IPC_SET:
            if (!sem_is_owner(set)) {
                ret = -EPERM;
                break;
            }
            set->uid = new_ds.sem_perm.uid;
            set->gid = new_ds.sem_perm.gid;
            set->mode = (unsigned short)(new_ds.sem_perm.mode & 0777);
            set->ctime = sem_now_seconds();
            break;
        case GETVAL:
        case GETPID:
        case GETNCNT:
        case GETZCNT:
            if (!sem_permitted(set, 0444)) {
                ret = -EACCES;
                break;
            }
            if (cmd == GETVAL) {
                ret = set->sems[semnum].value;
            } else if (cmd == GETPID) {
                ret = set->sems[semnum].pid;
            } else {
                long count = 0;
                for (struct sysv_sem_waiter* w = g_sem_waiters; w; w = w->next)
                    if (w->index == index && w->semnum == semnum && w->zero == (cmd == GETZCNT))
                        count++;
                ret = count;
            }
            break;
        case GETALL:
            if (!sem_permitted(set, 0444)) {
                ret = -EACCES;
                break;
            }
            values = malloc((size_t)nsems * sizeof(*values));
            if (!values) {
                ret = -ENOMEM;
                break;
            }
            for (int i = 0; i < nsems; i++)
                values[i] = set->sems[i].value;
            break;
        case SETVAL:
            if (!sem_permitted(set, 0222)) {
                ret = -EACCES;
                break;
            }
            if ((int)arg < 0 || (int)arg > SYSV_SEMVMX) {
                ret = -ERANGE;
                break;
            }
            set->sems[semnum].value = (unsigned short)(int)arg;
            set->sems[semnum].pid = g_process.pid;
            set->ctime = sem_now_seconds();
            sem_wake_waiters(index);
            break;
        case SETALL:
            if (!sem_permitted(set, 0222)) {
                ret = -EACCES;
                break;
            }
            if (!is_user_memory_readable(user_array, (size_t)nsems * sizeof(*user_array))) {
                ret = -EFAULT;
                break;
            }
            for (int i = 0; i < nsems; i++) {
                if (user_array[i] > SYSV_SEMVMX) {
                    ret = -ERANGE;
                    break;
                }
            }
            if (ret < 0)
                break;
            for (int i = 0; i < nsems; i++) {
                set->sems[i].value = user_array[i];
                set->sems[i].pid = g_process.pid;
            }
            set->ctime = sem_now_seconds();
            sem_wake_waiters(index);
            break;
        default:
            ret = -EINVAL;
            break;
    }
out:
    unlock(&g_sysv_sem_lock);
    if (values) {
        if (!is_user_memory_writable(user_array, (size_t)nsems * sizeof(*user_array)))
            ret = -EFAULT;
        else
            memcpy(user_array, values, (size_t)nsems * sizeof(*user_array));
        free(values);
    }
    return ret;
}

/* Try to apply all operations atomically. Returns 0 when applied, 1 when the caller must wait
 * (`*out_block` names the blocking operation), or a negative errno. */
static int sem_try_apply(struct sysv_sem_set* set, const struct linux_sembuf* sops, size_t nsops,
                         const struct linux_sembuf** out_block) {
    /* work on a copy of the values so a failing later operation undoes the earlier ones */
    for (size_t i = 0; i < nsops; i++) {
        struct sysv_sem* sem = &set->sems[sops[i].sem_num];
        int value = sem->value;
        int op = sops[i].sem_op;
        if (op == 0) {
            if (value != 0) {
                *out_block = &sops[i];
                goto undo_block;
            }
        } else if (op < 0) {
            if (value + op < 0) {
                *out_block = &sops[i];
                goto undo_block;
            }
            sem->value = (unsigned short)(value + op);
        } else {
            if (value + op > SYSV_SEMVMX) {
                for (size_t j = i; j-- > 0;)
                    set->sems[sops[j].sem_num].value -= (unsigned short)sops[j].sem_op;
                return -ERANGE;
            }
            sem->value = (unsigned short)(value + op);
        }
        continue;
undo_block:
        for (size_t j = i; j-- > 0;)
            set->sems[sops[j].sem_num].value -= (unsigned short)sops[j].sem_op;
        return 1;
    }
    for (size_t i = 0; i < nsops; i++)
        set->sems[sops[i].sem_num].pid = g_process.pid;
    set->otime = sem_now_seconds();
    return 0;
}

static long do_semop(int semid, const struct linux_sembuf* user_sops, size_t nsops,
                     uint64_t* timeout_us) {
    if (semid < 0 || nsops < 1)
        return -EINVAL;
    if (nsops > SYSV_SEMOPM)
        return -E2BIG;
    if (!is_user_memory_readable(user_sops, nsops * sizeof(*user_sops)))
        return -EFAULT;

    if (g_use_host_sysv_semaphores) {
        /* the host agent's protocol carries one operation */
        if (nsops != 1)
            return -E2BIG;
        struct linux_sembuf op = user_sops[0];
        for (;;) {
            long ret = host_sem_request(SYSV_SEMOP, semid, op.sem_num,
                                        (uint64_t)(int64_t)op.sem_op, op.sem_flg);
            if (ret != -EAGAIN || (op.sem_flg & IPC_NOWAIT))
                return ret;
            if (timeout_us && !*timeout_us)
                return -EAGAIN;
            uint64_t sleep_us = timeout_us ? MIN(*timeout_us, 1000) : 1000;
            ret = do_nanosleep(sleep_us, /*rem=*/NULL);
            if (ret < 0)
                return -ERESTARTSYS;
            if (timeout_us)
                *timeout_us -= sleep_us;
        }
    }

    struct linux_sembuf* sops = malloc(nsops * sizeof(*sops));
    if (!sops)
        return -ENOMEM;
    memcpy(sops, user_sops, nsops * sizeof(*sops));
    bool alter = false;
    unsigned int max_num = 0;
    for (size_t i = 0; i < nsops; i++) {
        if (sops[i].sem_num > max_num)
            max_num = sops[i].sem_num;
        if (sops[i].sem_op != 0)
            alter = true;
    }

    uint64_t deadline_us = 0;
    if (timeout_us) {
        uint64_t now = 0;
        if (PalSystemTimeQuery(&now) < 0) {
            free(sops);
            return -EINVAL;
        }
        deadline_us = now + *timeout_us;
    }

    bool slept = false;
    long ret;
    for (;;) {
        thread_prepare_wait();
        lock(&g_sysv_sem_lock);
        size_t index;
        struct sysv_sem_set* set = sem_lookup(semid, &index);
        if (!set) {
            ret = slept ? -EIDRM : -EINVAL;
            break;
        }
        if (max_num >= (unsigned int)set->nsems) {
            ret = -EFBIG;
            break;
        }
        if (!sem_permitted(set, alter ? 0222 : 0444)) {
            ret = -EACCES;
            break;
        }
        const struct linux_sembuf* block = NULL;
        int r = sem_try_apply(set, sops, nsops, &block);
        if (r <= 0) {
            ret = r;
            if (r == 0)
                sem_wake_waiters(index); /* values changed: others re-check */
            break;
        }
        if (block->sem_flg & IPC_NOWAIT) {
            ret = -EAGAIN;
            break;
        }
        uint64_t wait_us = 0;
        if (timeout_us) {
            uint64_t now = 0;
            if (PalSystemTimeQuery(&now) < 0 || now >= deadline_us) {
                ret = -EAGAIN;
                break;
            }
            wait_us = deadline_us - now;
        }
        struct sysv_sem_waiter waiter = {
            .next = g_sem_waiters, .thread = get_cur_thread(), .index = index,
            .semnum = block->sem_num, .zero = block->sem_op == 0,
        };
        g_sem_waiters = &waiter;
        unlock(&g_sysv_sem_lock);
        int wret = thread_wait(timeout_us ? &wait_us : NULL, /*ignore_pending_signals=*/false);
        lock(&g_sysv_sem_lock);
        sem_remove_waiter(&waiter);
        unlock(&g_sysv_sem_lock);
        slept = true;
        if (wret == -EINTR) {
            free(sops);
            return -ERESTARTNOHAND;
        }
    }
    unlock(&g_sysv_sem_lock);
    free(sops);
    return ret;
}

long libos_syscall_semop(int semid, const struct linux_sembuf* sops, size_t nsops) {
    /* --- guest-wide System V IPC (VM, sys.experimental__multiprocess): libos_sysv_shared.c --- */
    if (sysv_shared_sem_enabled())
        return sysv_shared_semtimedop(semid, sops, nsops, /*timeout=*/NULL);
    /* --- end of guest-wide System V IPC --- */
    return do_semop(semid, sops, nsops, /*timeout_us=*/NULL);
}

long libos_syscall_semtimedop(int semid, const struct linux_sembuf* sops, size_t nsops,
                              const struct __kernel_timespec* timeout) {
    /* --- guest-wide System V IPC (VM, sys.experimental__multiprocess): libos_sysv_shared.c --- */
    if (sysv_shared_sem_enabled())
        return sysv_shared_semtimedop(semid, sops, nsops, timeout);
    /* --- end of guest-wide System V IPC --- */
    if (!timeout)
        return do_semop(semid, sops, nsops, /*timeout_us=*/NULL);
    if (!is_user_memory_readable(timeout, sizeof(*timeout)))
        return -EFAULT;
    if (timeout->tv_sec < 0 || timeout->tv_nsec < 0 ||
            (uint64_t)timeout->tv_nsec >= TIME_NS_IN_S)
        return -EINVAL;

    uint64_t timeout_us = timespec_to_us(timeout);
    return do_semop(semid, sops, nsops, &timeout_us);
}

/* the four numbers of /proc/sys/kernel/sem: SEMMSL SEMMNS SEMOPM SEMMNI */
int sysv_sem_limits_load(char** out_data, size_t* out_size) {
    char* buf = malloc(64);
    if (!buf)
        return -ENOMEM;
    int n = snprintf(buf, 64, "%d\t%d\t%d\t%d\n", SYSV_SEMMSL, SYSV_SEMMNS, SYSV_SEMOPM,
                     SYSV_SEMMNI);
    if (n <= 0 || n >= 64) {
        free(buf);
        return -EINVAL;
    }
    *out_data = buf;
    *out_size = (size_t)n;
    return 0;
}
