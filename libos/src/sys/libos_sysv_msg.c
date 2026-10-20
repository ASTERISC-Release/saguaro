/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/*
 * System V message queues (msgget/msgsnd/msgrcv/msgctl).
 *
 * The queue table is process-local, like the System V shared memory and semaphore tables
 * (`libos_sysv_shm.c`, `libos_sem.c`): threads of one Gramine process share every queue, but a
 * child process (direct PAL `fork`) starts with an empty table, so a queue cannot carry messages
 * between processes. This limit is recorded in dev docs: libos-syscalls/TODO.md.
 *
 * Within the process the semantics follow the kernel (`ipc/msg.c`): identifiers are
 * `seq << 15 | index` so a removed queue's identifier stays invalid after the slot is reused,
 * the checks run in the kernel's order (so the first error a test expects is the one it gets),
 * blocked senders and receivers sleep on the thread's scheduler event and are woken by any change
 * of their queue, by its removal (`EIDRM`) or by a signal (`-ERESTARTNOHAND`, i.e. `EINTR` when a
 * handler runs, as on Linux), and `MSG_COPY`, `MSG_EXCEPT`, `MSG_NOERROR` and negative types work.
 */

#include "libos_internal.h"
#include "libos_sysv_shared.h"
#include "libos_lock.h"
#include "libos_process.h"
#include "libos_table.h"
#include "libos_thread.h"
#include "linux_abi/errors.h"
#include "linux_abi/ipc.h"
#include "pal.h"

/* Limits, published in /proc/sys/kernel/{msgmni,msgmax,msgmnb}. MSGMAX and MSGMNB are the
 * kernel's defaults; MSGMNI is smaller than the kernel's 32000 so the table stays small (LTP
 * msgget03 creates exactly that many queues and expects ENOSPC for the next one). */
#define SYSV_MSGMNI 1024
#define SYSV_MSGMAX 8192
#define SYSV_MSGMNB 16384
#define SYSV_MSG_IDX_BITS 15 /* the kernel's IPCMNI_SHIFT */

struct sysv_msg {
    struct sysv_msg* next;
    long type;
    size_t len;
    char text[];
};

struct sysv_msq {
    int key;
    unsigned short seq;
    unsigned short mode; /* IPC permission bits (0777) */
    unsigned int uid, gid, cuid, cgid;
    long stime, rtime, ctime;
    size_t cbytes, qnum, qbytes;
    int lspid, lrpid;
    struct sysv_msg* head;
    struct sysv_msg* tail;
};

/* A thread sleeping in msgsnd()/msgrcv(); lives on that thread's stack while it is listed. */
struct sysv_msg_waiter {
    struct sysv_msg_waiter* next;
    struct libos_thread* thread;
    size_t index;
};

static struct libos_lock g_sysv_msg_lock;
static struct sysv_msq* g_msqs[SYSV_MSGMNI];
static unsigned short g_msq_next_seq[SYSV_MSGMNI];
static size_t g_msq_in_use;
static struct sysv_msg_waiter* g_msg_waiters;

int init_sysv_msg(void) {
    return create_lock(&g_sysv_msg_lock) ? 0 : -ENOMEM;
}

unsigned int sysv_msg_limit(int which) {
    switch (which) {
        case 0:
            return SYSV_MSGMNI;
        case 1:
            return SYSV_MSGMAX;
        default:
            return SYSV_MSGMNB;
    }
}

static long msg_now_seconds(void) {
    uint64_t us = 0;
    if (PalSystemTimeQuery(&us) < 0)
        return 0;
    return (long)(us / 1000000ULL);
}

static int msq_id(const struct sysv_msq* q, size_t index) {
    return (int)(((unsigned int)q->seq << SYSV_MSG_IDX_BITS) | index);
}

/* The queue named by identifier `msqid`, or NULL (stale sequence number, free slot, bad id). */
static struct sysv_msq* msq_lookup(int msqid, size_t* out_index) {
    assert(locked(&g_sysv_msg_lock));
    if (msqid < 0)
        return NULL;
    size_t index = (unsigned int)msqid & ((1U << SYSV_MSG_IDX_BITS) - 1);
    unsigned int seq = (unsigned int)msqid >> SYSV_MSG_IDX_BITS;
    if (index >= SYSV_MSGMNI || !g_msqs[index] || g_msqs[index]->seq != seq)
        return NULL;
    *out_index = index;
    return g_msqs[index];
}

static bool msg_is_root(void) {
    return (unsigned int)libos_syscall_geteuid() == 0;
}

/* The kernel's `ipcperms()`: `flag` holds the requested rwx bits in any of the three classes;
 * the caller's class is owner (euid is the creator or owner), group, or other. Root has
 * CAP_IPC_OWNER and passes. */
static bool msq_permitted(const struct sysv_msq* q, unsigned int flag) {
    if (msg_is_root())
        return true;
    unsigned int euid = (unsigned int)libos_syscall_geteuid();
    unsigned int egid = (unsigned int)libos_syscall_getegid();
    unsigned int requested = ((flag >> 6) | (flag >> 3) | flag) & 07;
    unsigned int granted = q->mode;
    if (euid == q->cuid || euid == q->uid)
        granted >>= 6;
    else if (egid == q->cgid || egid == q->gid)
        granted >>= 3;
    return !(requested & ~granted & 07);
}

static bool msq_is_owner(const struct sysv_msq* q) {
    unsigned int euid = (unsigned int)libos_syscall_geteuid();
    return msg_is_root() || euid == q->cuid || euid == q->uid;
}

static void msg_wake_waiters(size_t index) {
    assert(locked(&g_sysv_msg_lock));
    for (struct sysv_msg_waiter* w = g_msg_waiters; w; w = w->next)
        if (w->index == index)
            thread_wakeup(w->thread);
}

static void msg_add_waiter(struct sysv_msg_waiter* waiter, size_t index) {
    assert(locked(&g_sysv_msg_lock));
    waiter->thread = get_cur_thread();
    waiter->index = index;
    waiter->next = g_msg_waiters;
    g_msg_waiters = waiter;
}

static void msg_remove_waiter(struct sysv_msg_waiter* waiter) {
    assert(locked(&g_sysv_msg_lock));
    for (struct sysv_msg_waiter** p = &g_msg_waiters; *p; p = &(*p)->next) {
        if (*p == waiter) {
            *p = waiter->next;
            return;
        }
    }
}

static void msq_free(size_t index) {
    assert(locked(&g_sysv_msg_lock));
    struct sysv_msq* q = g_msqs[index];
    struct sysv_msg* msg = q->head;
    while (msg) {
        struct sysv_msg* next = msg->next;
        free(msg);
        msg = next;
    }
    g_msqs[index] = NULL;
    g_msq_in_use--;
    free(q);
    /* sleepers find their identifier gone and answer EIDRM */
    msg_wake_waiters(index);
}

long libos_syscall_msgget(int key, int msgflg) {
    /* --- guest-wide System V IPC (VM, sys.experimental__multiprocess): libos_sysv_shared.c --- */
    if (sysv_shared_enabled())
        return sysv_shared_msgget(key, msgflg);
    /* --- end of guest-wide System V IPC --- */
    lock(&g_sysv_msg_lock);
    if (key != LINUX_IPC_PRIVATE) {
        for (size_t i = 0; i < SYSV_MSGMNI; i++) {
            struct sysv_msq* q = g_msqs[i];
            if (!q || q->key != key)
                continue;
            long ret;
            if ((msgflg & (LINUX_IPC_CREAT | LINUX_IPC_EXCL)) == (LINUX_IPC_CREAT | LINUX_IPC_EXCL))
                ret = -EEXIST;
            else if (!msq_permitted(q, (unsigned int)msgflg & 0777))
                ret = -EACCES;
            else
                ret = msq_id(q, i);
            unlock(&g_sysv_msg_lock);
            return ret;
        }
        if (!(msgflg & LINUX_IPC_CREAT)) {
            unlock(&g_sysv_msg_lock);
            return -ENOENT;
        }
    }

    size_t index = SYSV_MSGMNI;
    if (g_msq_in_use < SYSV_MSGMNI) {
        for (size_t i = 0; i < SYSV_MSGMNI; i++) {
            if (!g_msqs[i]) {
                index = i;
                break;
            }
        }
    }
    if (index == SYSV_MSGMNI) {
        unlock(&g_sysv_msg_lock);
        return -ENOSPC;
    }
    struct sysv_msq* q = malloc(sizeof(*q));
    if (!q) {
        unlock(&g_sysv_msg_lock);
        return -ENOMEM;
    }
    unsigned int euid = (unsigned int)libos_syscall_geteuid();
    unsigned int egid = (unsigned int)libos_syscall_getegid();
    *q = (struct sysv_msq){
        .key    = key,
        .seq    = g_msq_next_seq[index]++,
        .mode   = (unsigned short)(msgflg & 0777),
        .uid    = euid,
        .gid    = egid,
        .cuid   = euid,
        .cgid   = egid,
        .ctime  = msg_now_seconds(),
        .qbytes = SYSV_MSGMNB,
    };
    g_msqs[index] = q;
    g_msq_in_use++;
    long ret = msq_id(q, index);
    unlock(&g_sysv_msg_lock);
    return ret;
}

long libos_syscall_msgsnd(int msqid, const void* msgp, size_t msgsz, int msgflg) {
    /* --- guest-wide System V IPC (VM, sys.experimental__multiprocess): libos_sysv_shared.c --- */
    if (sysv_shared_enabled())
        return sysv_shared_msgsnd(msqid, msgp, msgsz, msgflg);
    /* --- end of guest-wide System V IPC --- */
    if (!is_user_memory_readable(msgp, sizeof(long)))
        return -EFAULT;
    long mtype = *(const long*)msgp;
    if (msgsz > SYSV_MSGMAX || (long)msgsz < 0 || msqid < 0)
        return -EINVAL;
    if (mtype < 1)
        return -EINVAL;
    const char* mtext = (const char*)msgp + sizeof(long);
    if (msgsz && !is_user_memory_readable(mtext, msgsz))
        return -EFAULT;

    struct sysv_msg* msg = malloc(sizeof(*msg) + msgsz);
    if (!msg)
        return -ENOMEM;
    msg->next = NULL;
    msg->type = mtype;
    msg->len = msgsz;
    memcpy(msg->text, mtext, msgsz);

    bool slept = false;
    for (;;) {
        thread_prepare_wait();
        lock(&g_sysv_msg_lock);
        size_t index;
        struct sysv_msq* q = msq_lookup(msqid, &index);
        long ret = 0;
        if (!q) {
            ret = slept ? -EIDRM : -EINVAL;
        } else if (!msq_permitted(q, 0222)) {
            ret = -EACCES;
        } else if (msgsz + q->cbytes <= q->qbytes && 1 + q->qnum <= q->qbytes) {
            if (q->tail)
                q->tail->next = msg;
            else
                q->head = msg;
            q->tail = msg;
            q->cbytes += msgsz;
            q->qnum++;
            q->lspid = g_process.pid;
            q->stime = msg_now_seconds();
            msg = NULL;
            msg_wake_waiters(index); /* receivers waiting for a message */
        } else if (msgflg & LINUX_IPC_NOWAIT) {
            ret = -EAGAIN;
        } else {
            struct sysv_msg_waiter waiter;
            msg_add_waiter(&waiter, index);
            unlock(&g_sysv_msg_lock);
            int wret = thread_wait(/*timeout_us=*/NULL, /*ignore_pending_signals=*/false);
            lock(&g_sysv_msg_lock);
            msg_remove_waiter(&waiter);
            unlock(&g_sysv_msg_lock);
            slept = true;
            if (wret == -EINTR) {
                free(msg);
                return -ERESTARTNOHAND;
            }
            continue;
        }
        unlock(&g_sysv_msg_lock);
        free(msg);
        return ret;
    }
}

/* The kernel's `find_msg()` over one queue. `mode` selects how `type` is matched. */
enum msg_search { SEARCH_ANY, SEARCH_EQUAL, SEARCH_NOTEQUAL, SEARCH_LESSEQUAL, SEARCH_NUMBER };

static struct sysv_msg* msq_find(struct sysv_msq* q, long type, enum msg_search mode,
                                 struct sysv_msg** out_prev) {
    struct sysv_msg* found = NULL;
    struct sysv_msg* found_prev = NULL;
    struct sysv_msg* prev = NULL;
    long count = 0;
    for (struct sysv_msg* msg = q->head; msg; prev = msg, msg = msg->next) {
        bool match;
        switch (mode) {
            case SEARCH_ANY:
                match = true;
                break;
            case SEARCH_EQUAL:
                match = msg->type == type;
                break;
            case SEARCH_NOTEQUAL:
                match = msg->type != type;
                break;
            case SEARCH_LESSEQUAL:
                match = msg->type <= type;
                break;
            case SEARCH_NUMBER:
            default:
                match = count++ == type;
                break;
        }
        if (!match)
            continue;
        if (mode == SEARCH_LESSEQUAL && msg->type != 1) {
            /* keep looking for a smaller type; the first message of the smallest type wins */
            type = msg->type - 1;
            found = msg;
            found_prev = prev;
            continue;
        }
        found = msg;
        found_prev = prev;
        break;
    }
    *out_prev = found_prev;
    return found;
}

long libos_syscall_msgrcv(int msqid, void* msgp, size_t msgsz, long msgtyp, int msgflg) {
    /* --- guest-wide System V IPC (VM, sys.experimental__multiprocess): libos_sysv_shared.c --- */
    if (sysv_shared_enabled())
        return sysv_shared_msgrcv(msqid, msgp, msgsz, msgtyp, msgflg);
    /* --- end of guest-wide System V IPC --- */
    if (msqid < 0 || (long)msgsz < 0)
        return -EINVAL;
    if (msgflg & LINUX_MSG_COPY) {
        if ((msgflg & LINUX_MSG_EXCEPT) || !(msgflg & LINUX_IPC_NOWAIT))
            return -EINVAL;
        /* the kernel reads the caller's buffer up front for MSG_COPY (`prepare_copy()`) */
        size_t copy_size = MIN(msgsz, (size_t)SYSV_MSGMAX);
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

    bool slept = false;
    struct sysv_msg* msg = NULL;
    bool copied = false; /* MSG_COPY: `msg` is still on the queue, copy it out under the lock */
    for (;;) {
        thread_prepare_wait();
        lock(&g_sysv_msg_lock);
        size_t index;
        struct sysv_msq* q = msq_lookup(msqid, &index);
        long ret = 0;
        if (!q) {
            ret = slept ? -EIDRM : -EINVAL;
        } else if (!msq_permitted(q, 0444)) {
            ret = -EACCES;
        } else {
            struct sysv_msg* prev;
            struct sysv_msg* found = msq_find(q, msgtyp, mode, &prev);
            if (found) {
                if (msgsz < found->len && !(msgflg & LINUX_MSG_NOERROR)) {
                    ret = -E2BIG;
                } else if (msgflg & LINUX_MSG_COPY) {
                    if (MIN(msgsz, (size_t)SYSV_MSGMAX) < found->len) {
                        ret = -EINVAL; /* the kernel's `copy_msg()` into a smaller copy */
                    } else {
                        /* copy while holding the lock: the message stays on the queue */
                        size_t n = found->len;
                        if (!is_user_memory_writable(msgp, sizeof(long) + n)) {
                            ret = -EFAULT;
                        } else {
                            *(long*)msgp = found->type;
                            memcpy((char*)msgp + sizeof(long), found->text, n);
                            ret = (long)n;
                        }
                        copied = true;
                    }
                } else {
                    if (prev)
                        prev->next = found->next;
                    else
                        q->head = found->next;
                    if (q->tail == found)
                        q->tail = prev;
                    q->cbytes -= found->len;
                    q->qnum--;
                    q->lrpid = g_process.pid;
                    q->rtime = msg_now_seconds();
                    msg = found;
                    msg_wake_waiters(index); /* senders waiting for room */
                }
            } else if (msgflg & LINUX_IPC_NOWAIT) {
                ret = -ENOMSG;
            } else {
                struct sysv_msg_waiter waiter;
                msg_add_waiter(&waiter, index);
                unlock(&g_sysv_msg_lock);
                int wret = thread_wait(/*timeout_us=*/NULL, /*ignore_pending_signals=*/false);
                lock(&g_sysv_msg_lock);
                msg_remove_waiter(&waiter);
                unlock(&g_sysv_msg_lock);
                slept = true;
                if (wret == -EINTR)
                    return -ERESTARTNOHAND;
                continue;
            }
        }
        unlock(&g_sysv_msg_lock);
        if (!msg || copied)
            return ret;
        break;
    }

    /* As in the kernel, a message that cannot be stored is lost (it left the queue already). */
    size_t n = MIN(msgsz, msg->len);
    long ret;
    if (!is_user_memory_writable(msgp, sizeof(long) + n)) {
        ret = -EFAULT;
    } else {
        *(long*)msgp = msg->type;
        memcpy((char*)msgp + sizeof(long), msg->text, n);
        ret = (long)n;
    }
    free(msg);
    return ret;
}

static void msq_fill_ds(const struct sysv_msq* q, struct linux_msqid_ds* ds) {
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

long libos_syscall_msgctl(int msqid, int cmd, void* buf) {
    /* --- guest-wide System V IPC (VM, sys.experimental__multiprocess): libos_sysv_shared.c --- */
    if (sysv_shared_enabled())
        return sysv_shared_msgctl(msqid, cmd, buf);
    /* --- end of guest-wide System V IPC --- */
    if (msqid < 0 || cmd < 0)
        return -EINVAL;

    switch (cmd) {
        case LINUX_IPC_INFO:
        case LINUX_MSG_INFO: {
            struct linux_msginfo info = {
                .msgmni = SYSV_MSGMNI,
                .msgmax = SYSV_MSGMAX,
                .msgmnb = SYSV_MSGMNB,
                .msgssz = 16,         /* the kernel's MSGSSZ */
                .msgseg = 0xffff,     /* the kernel's MSGSEG, clamped to its field */
            };
            lock(&g_sysv_msg_lock);
            long max_index = 0;
            size_t messages = 0, bytes = 0;
            for (size_t i = 0; i < SYSV_MSGMNI; i++) {
                if (!g_msqs[i])
                    continue;
                max_index = (long)i;
                messages += g_msqs[i]->qnum;
                bytes += g_msqs[i]->cbytes;
            }
            if (cmd == LINUX_MSG_INFO) {
                info.msgpool = (int)g_msq_in_use;
                info.msgmap  = (int)messages;
                info.msgtql  = (int)bytes;
            } else {
                info.msgpool = (SYSV_MSGMNI * SYSV_MSGMNB) / 1024; /* the kernel's MSGPOOL */
                info.msgmap  = SYSV_MSGMNB;                       /* MSGMAP */
                info.msgtql  = SYSV_MSGMNB;                       /* MSGTQL */
            }
            unlock(&g_sysv_msg_lock);
            if (!is_user_memory_writable(buf, sizeof(info)))
                return -EFAULT;
            *(struct linux_msginfo*)buf = info;
            return max_index;
        }
        case LINUX_IPC_STAT:
        case LINUX_MSG_STAT:
        case LINUX_MSG_STAT_ANY: {
            struct linux_msqid_ds ds;
            long ret;
            lock(&g_sysv_msg_lock);
            struct sysv_msq* q = NULL;
            size_t index;
            if (cmd == LINUX_IPC_STAT) {
                q = msq_lookup(msqid, &index);
            } else if ((size_t)msqid < SYSV_MSGMNI) {
                index = (size_t)msqid; /* MSG_STAT takes an index, not an identifier */
                q = g_msqs[index];
            }
            if (!q) {
                ret = -EINVAL;
            } else if (cmd != LINUX_MSG_STAT_ANY && !msq_permitted(q, 0444)) {
                ret = -EACCES;
            } else {
                msq_fill_ds(q, &ds);
                ret = cmd == LINUX_IPC_STAT ? 0 : msq_id(q, index);
            }
            unlock(&g_sysv_msg_lock);
            if (ret < 0)
                return ret;
            if (!is_user_memory_writable(buf, sizeof(ds)))
                return -EFAULT;
            *(struct linux_msqid_ds*)buf = ds;
            return ret;
        }
        case LINUX_IPC_SET: {
            if (!is_user_memory_readable(buf, sizeof(struct linux_msqid_ds)))
                return -EFAULT;
            struct linux_msqid_ds ds = *(struct linux_msqid_ds*)buf;
            lock(&g_sysv_msg_lock);
            size_t index;
            struct sysv_msq* q = msq_lookup(msqid, &index);
            long ret = 0;
            if (!q) {
                ret = -EINVAL;
            } else if (!msq_is_owner(q)) {
                ret = -EPERM;
            } else if (ds.msg_qbytes > SYSV_MSGMNB && !msg_is_root()) {
                ret = -EPERM; /* raising the limit needs CAP_SYS_RESOURCE */
            } else {
                q->qbytes = ds.msg_qbytes;
                q->uid = ds.msg_perm.uid;
                q->gid = ds.msg_perm.gid;
                q->mode = (unsigned short)(ds.msg_perm.mode & 0777);
                q->ctime = msg_now_seconds();
                /* more room, or different permissions: every sleeper re-checks */
                msg_wake_waiters(index);
            }
            unlock(&g_sysv_msg_lock);
            return ret;
        }
        case LINUX_IPC_RMID: {
            lock(&g_sysv_msg_lock);
            size_t index;
            struct sysv_msq* q = msq_lookup(msqid, &index);
            long ret = 0;
            if (!q)
                ret = -EINVAL;
            else if (!msq_is_owner(q))
                ret = -EPERM;
            else
                msq_free(index);
            unlock(&g_sysv_msg_lock);
            return ret;
        }
        default:
            return -EINVAL;
    }
}

/* Content of `/proc/sysvipc/msg`, in the kernel's format (`sysvipc_msg_proc_show()`); LTP's IPC
 * library counts its lines to learn how many queues exist. */
int sysv_msg_proc_load(char** out_data, size_t* out_size) {
    /* --- guest-wide System V IPC (VM, sys.experimental__multiprocess): libos_sysv_shared.c --- */
    if (sysv_shared_enabled())
        return sysv_shared_msg_proc_load(out_data, out_size);
    /* --- end of guest-wide System V IPC --- */
    static const char header[] =
        "       key      msqid perms      cbytes       qnum lspid lrpid   uid   gid  cuid  cgid"
        "      stime      rtime      ctime\n";
    lock(&g_sysv_msg_lock);
    size_t cap = sizeof(header) + g_msq_in_use * 160;
    char* buf = malloc(cap);
    if (!buf) {
        unlock(&g_sysv_msg_lock);
        return -ENOMEM;
    }
    size_t off = sizeof(header) - 1;
    memcpy(buf, header, off);
    for (size_t i = 0; i < SYSV_MSGMNI; i++) {
        struct sysv_msq* q = g_msqs[i];
        if (!q)
            continue;
        int n = snprintf(buf + off, cap - off,
                         "%10d %10d  %4o  %10zu %10zu %5d %5d %5u %5u %5u %5u %10ld %10ld %10ld\n",
                         q->key, msq_id(q, i), q->mode, q->cbytes, q->qnum, q->lspid, q->lrpid,
                         q->uid, q->gid, q->cuid, q->cgid, q->stime, q->rtime, q->ctime);
        assert(n > 0 && (size_t)n < cap - off);
        off += (size_t)n;
    }
    unlock(&g_sysv_msg_lock);
    *out_data = buf;
    *out_size = off;
    return 0;
}
