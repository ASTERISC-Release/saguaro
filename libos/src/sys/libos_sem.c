/* SPDX-License-Identifier: LGPL-3.0-or-later */

#include "libos_lock.h"
#include "libos_internal.h"
#include "libos_sg.h"
#include "libos_signal.h"
#include "libos_table.h"
#include "libos_utils.h"
#include "linux_abi/errors.h"
#include "pal.h"
#include "toml_utils.h"

#define IPC_CREAT 01000
#define IPC_EXCL 02000
#define IPC_NOWAIT 04000
#define SEM_UNDO 010000
#define MAX_SYSV_SEM_SETS 256
#define IPC_RMID 0
#define GETPID 11
#define GETVAL 12
#define GETNCNT 14
#define GETZCNT 15
#define SETVAL 16

struct linux_sembuf {
    unsigned short sem_num;
    short sem_op;
    short sem_flg;
};

struct sysv_sem_set {
    bool used;
    bool removed;
    int key;
    int nsems;
    unsigned short value;
    int last_pid;
    size_t decrement_waiters;
    size_t zero_waiters;
    PAL_HANDLE changed_event;
};

static struct libos_lock g_sysv_sem_lock;
static struct sysv_sem_set g_sysv_sem_sets[MAX_SYSV_SEM_SETS];
static bool g_use_host_sysv_semaphores;

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

int init_sysv_semaphores(void) {
    int ret = toml_bool_in(g_manifest_root, "sys.experimental__enable_host_sysv_semaphores",
                           /*defaultval=*/false, &g_use_host_sysv_semaphores);
    if (ret < 0) {
        log_error("Cannot parse 'sys.experimental__enable_host_sysv_semaphores'");
        return -EINVAL;
    }
    return create_lock(&g_sysv_sem_lock) ? 0 : -ENOMEM;
}

long libos_syscall_semget(int key, int nsems, int semflg) {
    if (g_use_host_sysv_semaphores)
        return host_sem_request(SYSV_SEMGET, key, nsems, semflg, 0);

    /* TF2 uses one-element sets as process-local thread semaphores. */
    if (nsems != 1)
        return -EINVAL;

    lock(&g_sysv_sem_lock);

    size_t free_index = MAX_SYSV_SEM_SETS;
    for (size_t i = 0; i < MAX_SYSV_SEM_SETS; i++) {
        if (!g_sysv_sem_sets[i].used) {
            if (free_index == MAX_SYSV_SEM_SETS)
                free_index = i;
            continue;
        }

        if (key != 0 && g_sysv_sem_sets[i].key == key) {
            if ((semflg & (IPC_CREAT | IPC_EXCL)) == (IPC_CREAT | IPC_EXCL)) {
                unlock(&g_sysv_sem_lock);
                return -EEXIST;
            }
            if (nsems > g_sysv_sem_sets[i].nsems) {
                unlock(&g_sysv_sem_lock);
                return -EINVAL;
            }
            unlock(&g_sysv_sem_lock);
            return i;
        }
    }

    if (!(semflg & IPC_CREAT)) {
        unlock(&g_sysv_sem_lock);
        return -ENOENT;
    }
    if (free_index == MAX_SYSV_SEM_SETS) {
        unlock(&g_sysv_sem_lock);
        return -ENOSPC;
    }

    PAL_HANDLE changed_event = g_sysv_sem_sets[free_index].changed_event;
    if (!changed_event) {
        if (PalEventCreate(&changed_event, /*init_signaled=*/false, /*auto_clear=*/true) < 0) {
            unlock(&g_sysv_sem_lock);
            return -ENOMEM;
        }
    } else {
        PalEventClear(changed_event);
    }

    g_sysv_sem_sets[free_index] = (struct sysv_sem_set){
        .used  = true,
        .key   = key,
        .nsems = nsems,
        .changed_event = changed_event,
    };
    unlock(&g_sysv_sem_lock);
    return free_index;
}

long libos_syscall_semctl(int semid, int semnum, int cmd, unsigned long arg) {
    if (g_use_host_sysv_semaphores)
        return host_sem_request(SYSV_SEMCTL, semid, semnum, cmd, arg);

    if (semid < 0 || semid >= MAX_SYSV_SEM_SETS)
        return -EINVAL;

    lock(&g_sysv_sem_lock);
    struct sysv_sem_set* set = &g_sysv_sem_sets[semid];
    if (!set->used || set->removed || semnum < 0 || semnum >= set->nsems) {
        unlock(&g_sysv_sem_lock);
        return -EINVAL;
    }

    if (cmd == IPC_RMID) {
        set->removed = true;
        PalEventSet(set->changed_event);
        unlock(&g_sysv_sem_lock);
        return 0;
    }
    if (cmd == SETVAL) {
        if (arg > 32767) {
            unlock(&g_sysv_sem_lock);
            return -ERANGE;
        }
        set->value = arg;
        set->last_pid = libos_syscall_getpid();
        PalEventSet(set->changed_event);
        unlock(&g_sysv_sem_lock);
        return 0;
    }
    if (cmd == GETVAL) {
        unsigned short value = set->value;
        unlock(&g_sysv_sem_lock);
        return value;
    }
    if (cmd == GETPID) {
        int last_pid = set->last_pid;
        unlock(&g_sysv_sem_lock);
        return last_pid;
    }
    if (cmd == GETNCNT) {
        size_t waiters = set->decrement_waiters;
        unlock(&g_sysv_sem_lock);
        return waiters;
    }
    if (cmd == GETZCNT) {
        size_t waiters = set->zero_waiters;
        unlock(&g_sysv_sem_lock);
        return waiters;
    }

    unlock(&g_sysv_sem_lock);
    log_warning("semctl probe: semid=%d semnum=%d cmd=%d arg=%#lx", semid, semnum, cmd, arg);
    return -ENOSYS;
}

static long do_semop(int semid, const struct linux_sembuf* sops, size_t nsops,
                     uint64_t* timeout_us) {
    if (!nsops)
        return -EINVAL;
    /* The initial implementation intentionally supports TF2's single-operation semaphore use. */
    if (nsops != 1)
        return -E2BIG;
    if (!is_user_memory_readable(sops, sizeof(*sops)))
        return -EFAULT;
    if (semid < 0)
        return -EINVAL;

    struct linux_sembuf op = sops[0];
    if (op.sem_num != 0 || (op.sem_flg & ~(IPC_NOWAIT | SEM_UNDO)))
        return -EINVAL;

    if (g_use_host_sysv_semaphores) {
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

    if (semid >= MAX_SYSV_SEM_SETS)
        return -EINVAL;

    bool registered_waiter = false;
    for (;;) {
        lock(&g_sysv_sem_lock);
        struct sysv_sem_set* set = &g_sysv_sem_sets[semid];
        if (!set->used || set->removed) {
            if (registered_waiter) {
                if (op.sem_op < 0)
                    set->decrement_waiters--;
                else
                    set->zero_waiters--;
                if (set->decrement_waiters || set->zero_waiters)
                    PalEventSet(set->changed_event);
            }
            unlock(&g_sysv_sem_lock);
            return -EIDRM;
        }

        bool can_apply;
        if (op.sem_op < 0) {
            can_apply = set->value >= (unsigned int)-op.sem_op;
        } else if (op.sem_op == 0) {
            can_apply = set->value == 0;
        } else {
            can_apply = set->value <= 32767 - op.sem_op;
            if (!can_apply) {
                unlock(&g_sysv_sem_lock);
                return -ERANGE;
            }
        }

        if (can_apply) {
            if (registered_waiter) {
                if (op.sem_op < 0)
                    set->decrement_waiters--;
                else
                    set->zero_waiters--;
            }
            set->value += op.sem_op;
            set->last_pid = libos_syscall_getpid();

            if ((set->value > 0 && set->decrement_waiters) ||
                    (set->value == 0 && set->zero_waiters)) {
                PalEventSet(set->changed_event);
            }
            unlock(&g_sysv_sem_lock);
            return 0;
        }

        if (op.sem_flg & IPC_NOWAIT) {
            unlock(&g_sysv_sem_lock);
            return -EAGAIN;
        }

        if (!registered_waiter) {
            if (op.sem_op < 0)
                set->decrement_waiters++;
            else
                set->zero_waiters++;
            registered_waiter = true;
        }
        PAL_HANDLE changed_event = set->changed_event;
        unlock(&g_sysv_sem_lock);

        int ret = PalEventWait(changed_event, timeout_us);
        if (ret == -PAL_ERROR_TRYAGAIN) {
            lock(&g_sysv_sem_lock);
            set = &g_sysv_sem_sets[semid];
            if (op.sem_op < 0)
                set->decrement_waiters--;
            else
                set->zero_waiters--;
            unlock(&g_sysv_sem_lock);
            return -EAGAIN;
        }
        if (ret < 0 && ret != -PAL_ERROR_TRYAGAIN) {
            lock(&g_sysv_sem_lock);
            set = &g_sysv_sem_sets[semid];
            if (op.sem_op < 0)
                set->decrement_waiters--;
            else
                set->zero_waiters--;
            unlock(&g_sysv_sem_lock);
            return pal_to_unix_errno(ret);
        }
    }
}

long libos_syscall_semop(int semid, const struct linux_sembuf* sops, size_t nsops) {
    return do_semop(semid, sops, nsops, /*timeout_us=*/NULL);
}

long libos_syscall_semtimedop(int semid, const struct linux_sembuf* sops, size_t nsops,
                              const struct __kernel_timespec* timeout) {
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
