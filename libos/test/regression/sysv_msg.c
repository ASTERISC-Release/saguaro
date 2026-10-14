/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/*
 * Regression tests for System V message queues: msgget, msgsnd, msgrcv, msgctl.
 *
 * Everything runs in one process (threads instead of fork, `seteuid` for the permission checks),
 * so it also runs under the single-process VM PAL: key lookup and creation errors, the receive
 * selection rules (type 0, a positive type, MSG_EXCEPT, a negative type, MSG_COPY), truncation
 * with MSG_NOERROR and E2BIG without it, a full queue (EAGAIN) and IPC_SET of `msg_qbytes`, the
 * IPC_STAT bookkeeping, a stale identifier after IPC_RMID, a receiver blocked in another thread
 * woken by a send, by IPC_RMID (EIDRM) and by a signal (EINTR), the IPC_INFO/MSG_INFO/MSG_STAT
 * queries, and `/proc/sysvipc/msg` and `/proc/sys/kernel/msgmni`.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#ifndef MSG_COPY
#define MSG_COPY 040000
#endif
#ifndef MSG_STAT_ANY
#define MSG_STAT_ANY 13
#endif

#define CHECK(cond)                                                \
    do {                                                           \
        if (!(cond)) {                                             \
            fprintf(stderr, "%s:%d: %s failed (errno=%d)\n",       \
                    __FILE__, __LINE__, #cond, errno);             \
            exit(1);                                               \
        }                                                          \
    } while (0)

struct msgbuf_test {
    long mtype;
    char mtext[64];
};

static int snd(int id, long type, const char* text, int flags) {
    struct msgbuf_test m = {.mtype = type};
    size_t len = strlen(text) + 1;
    memcpy(m.mtext, text, len);
    return msgsnd(id, &m, len, flags);
}

static void sleep_ms(long ms) {
    struct timespec ts = {.tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000L};
    while (nanosleep(&ts, &ts) < 0 && errno == EINTR)
        ;
}

static int g_qid;
static volatile int g_thread_ret;
static volatile int g_thread_errno;
static struct msgbuf_test g_thread_msg;

static void* receiver(void* arg) {
    (void)arg;
    errno = 0;
    g_thread_ret = (int)msgrcv(g_qid, &g_thread_msg, sizeof(g_thread_msg.mtext), 0, 0);
    g_thread_errno = errno;
    return NULL;
}

static void on_signal(int sig) {
    (void)sig;
}

static void start_receiver(pthread_t* t) {
    g_thread_ret = -2;
    g_thread_errno = 0;
    CHECK(pthread_create(t, NULL, receiver, NULL) == 0);
    sleep_ms(200); /* let it block */
    CHECK(g_thread_ret == -2);
}

int main(void) {
    struct msgbuf_test m;

    /* ---- creation and lookup ---- */
    const key_t key = 0x5e1f0001;
    int id = msgget(key, IPC_CREAT | IPC_EXCL | 0600);
    CHECK(id >= 0);
    CHECK(msgget(key, 0) == id);
    CHECK(msgget(key, IPC_CREAT | IPC_EXCL | 0600) == -1 && errno == EEXIST);
    CHECK(msgget(key + 1, 0600) == -1 && errno == ENOENT);
    int id_priv = msgget(IPC_PRIVATE, 0600); /* IPC_PRIVATE creates even without IPC_CREAT */
    CHECK(id_priv >= 0 && id_priv != id);

    /* ---- argument checks of msgsnd/msgrcv ---- */
    CHECK(snd(id, 0, "x", 0) == -1 && errno == EINVAL); /* mtype must be > 0 */
    CHECK(msgsnd(id, &m, (size_t)-1, 0) == -1 && errno == EINVAL);
    CHECK(msgsnd(-1, &m, 1, 0) == -1 && errno == EINVAL);
    CHECK(msgsnd(id, NULL, 1, 0) == -1 && errno == EFAULT);
    CHECK(msgrcv(id, &m, sizeof(m.mtext), 0, IPC_NOWAIT) == -1 && errno == ENOMSG);
    CHECK(msgrcv(id, &m, sizeof(m.mtext), 0, MSG_COPY) == -1 && errno == EINVAL);
    CHECK(msgrcv(id, &m, sizeof(m.mtext), 0, MSG_COPY | MSG_EXCEPT | IPC_NOWAIT) == -1
          && errno == EINVAL);

    /* ---- selection rules ---- */
    CHECK(snd(id, 3, "three", 0) == 0);
    CHECK(snd(id, 1, "one", 0) == 0);
    CHECK(snd(id, 2, "two", 0) == 0);
    CHECK(snd(id, 1, "one-b", 0) == 0);

    struct msqid_ds ds;
    CHECK(msgctl(id, IPC_STAT, &ds) == 0);
    CHECK(ds.msg_qnum == 4);
    CHECK(ds.msg_cbytes == sizeof("three") + sizeof("one") + sizeof("two") + sizeof("one-b"));
    CHECK(ds.msg_lspid == getpid() && ds.msg_lrpid == 0);
    CHECK(ds.msg_stime != 0 && ds.msg_rtime == 0);
    CHECK((ds.msg_perm.mode & 0777) == 0600);

    /* MSG_COPY: the n-th message, left on the queue */
    memset(&m, 0, sizeof(m));
    CHECK(msgrcv(id, &m, sizeof(m.mtext), 2, MSG_COPY | IPC_NOWAIT) == (ssize_t)sizeof("two"));
    CHECK(m.mtype == 2 && strcmp(m.mtext, "two") == 0);
    CHECK(msgrcv(id, &m, sizeof(m.mtext), 9, MSG_COPY | IPC_NOWAIT) == -1 && errno == ENOMSG);
    CHECK(msgctl(id, IPC_STAT, &ds) == 0 && ds.msg_qnum == 4);

    /* a too-small buffer: E2BIG without MSG_NOERROR, the message stays */
    CHECK(msgrcv(id, &m, 2, 3, IPC_NOWAIT) == -1 && errno == E2BIG);
    /* with MSG_NOERROR: truncated, consumed */
    memset(&m, 0, sizeof(m));
    CHECK(msgrcv(id, &m, 2, 3, IPC_NOWAIT | MSG_NOERROR) == 2);
    CHECK(m.mtype == 3 && memcmp(m.mtext, "th", 2) == 0 && m.mtext[2] == 0);

    /* MSG_EXCEPT 1: the first message of another type */
    CHECK(msgrcv(id, &m, sizeof(m.mtext), 1, MSG_EXCEPT | IPC_NOWAIT) > 0);
    CHECK(m.mtype == 2 && strcmp(m.mtext, "two") == 0);
    CHECK(snd(id, 5, "five", 0) == 0);
    CHECK(snd(id, 4, "four", 0) == 0);
    /* negative type: the lowest type <= |type|, first of its kind */
    CHECK(msgrcv(id, &m, sizeof(m.mtext), -5, IPC_NOWAIT) > 0);
    CHECK(m.mtype == 1 && strcmp(m.mtext, "one") == 0);
    /* positive type: exactly that type */
    CHECK(msgrcv(id, &m, sizeof(m.mtext), 4, IPC_NOWAIT) > 0);
    CHECK(m.mtype == 4 && strcmp(m.mtext, "four") == 0);
    /* type 0: queue order */
    CHECK(msgrcv(id, &m, sizeof(m.mtext), 0, IPC_NOWAIT) > 0 && strcmp(m.mtext, "one-b") == 0);
    CHECK(msgrcv(id, &m, sizeof(m.mtext), 0, IPC_NOWAIT) > 0 && strcmp(m.mtext, "five") == 0);
    CHECK(msgrcv(id, &m, sizeof(m.mtext), 0, IPC_NOWAIT) == -1 && errno == ENOMSG);
    CHECK(msgctl(id, IPC_STAT, &ds) == 0);
    CHECK(ds.msg_qnum == 0 && ds.msg_cbytes == 0 && ds.msg_lrpid == getpid());

    /* ---- a full queue, and IPC_SET of msg_qbytes ---- */
    ds.msg_qbytes = 16;
    CHECK(msgctl(id, IPC_SET, &ds) == 0);
    CHECK(msgctl(id, IPC_STAT, &ds) == 0 && ds.msg_qbytes == 16);
    CHECK(snd(id, 1, "0123456789", IPC_NOWAIT) == 0); /* 11 bytes */
    CHECK(snd(id, 1, "01234", IPC_NOWAIT) == -1 && errno == EAGAIN); /* 11 + 6 > 16 */
    CHECK(snd(id, 1, "0123", IPC_NOWAIT) == 0); /* 11 + 5 == 16 */
    CHECK(msgrcv(id, &m, sizeof(m.mtext), 0, IPC_NOWAIT) > 0);
    CHECK(msgrcv(id, &m, sizeof(m.mtext), 0, IPC_NOWAIT) > 0);

    /* ---- blocking receive in another thread ---- */
    g_qid = id;
    pthread_t t;
    start_receiver(&t);
    CHECK(snd(id, 7, "wake", 0) == 0);
    CHECK(pthread_join(t, NULL) == 0);
    CHECK(g_thread_ret == (int)sizeof("wake") && g_thread_msg.mtype == 7);
    CHECK(strcmp(g_thread_msg.mtext, "wake") == 0);

    /* a signal interrupts it with EINTR (even with SA_RESTART, as on Linux) */
    struct sigaction sa = {.sa_handler = on_signal, .sa_flags = SA_RESTART};
    sigemptyset(&sa.sa_mask);
    CHECK(sigaction(SIGUSR1, &sa, NULL) == 0);
    start_receiver(&t);
    CHECK(pthread_kill(t, SIGUSR1) == 0);
    CHECK(pthread_join(t, NULL) == 0);
    CHECK(g_thread_ret == -1 && g_thread_errno == EINTR);

    /* ---- the queries ---- */
    struct msginfo info;
    int max_index = msgctl(id, IPC_INFO, (struct msqid_ds*)&info);
    CHECK(max_index >= 0);
    CHECK(info.msgmax == 8192 && info.msgmnb == 16384 && info.msgmni > 0);
    CHECK(msgctl(id, MSG_INFO, (struct msqid_ds*)&info) == max_index);
    CHECK(info.msgpool >= 2); /* queues in use */
    int found = 0;
    for (int i = 0; i <= max_index; i++) {
        int qid = msgctl(i, MSG_STAT, &ds);
        if (qid == id) {
            CHECK(ds.msg_perm.__key == key);
            found = 1;
        }
    }
    CHECK(found);

    FILE* f = fopen("/proc/sys/kernel/msgmni", "r");
    CHECK(f);
    int msgmni = 0;
    CHECK(fscanf(f, "%d", &msgmni) == 1 && msgmni == info.msgmni);
    fclose(f);

    f = fopen("/proc/sysvipc/msg", "r");
    CHECK(f);
    char line[256];
    int lines = 0, has_id = 0;
    while (fgets(line, sizeof(line), f)) {
        int k, qid;
        if (lines++ && sscanf(line, "%d %d", &k, &qid) == 2 && qid == id && k == key)
            has_id = 1;
    }
    fclose(f);
    CHECK(lines >= 3 && has_id); /* header + two queues */

    /* ---- permissions, by dropping effective privileges ---- */
    CHECK(seteuid(65534 /*nobody*/) == 0);
    CHECK(snd(id, 1, "x", IPC_NOWAIT) == -1 && errno == EACCES);
    CHECK(msgrcv(id, &m, sizeof(m.mtext), 0, IPC_NOWAIT) == -1 && errno == EACCES);
    CHECK(msgctl(id, IPC_STAT, &ds) == -1 && errno == EACCES);
    CHECK(msgctl(id, IPC_RMID, NULL) == -1 && errno == EPERM);
    CHECK(msgget(key, 0600) == -1 && errno == EACCES);
    CHECK(msgget(key, 0) == id); /* asking for no access always works */
    CHECK(seteuid(0) == 0);

    /* ---- IPC_RMID wakes a blocked receiver with EIDRM; the identifier is then stale ---- */
    start_receiver(&t);
    CHECK(msgctl(id, IPC_RMID, NULL) == 0);
    CHECK(pthread_join(t, NULL) == 0);
    CHECK(g_thread_ret == -1 && g_thread_errno == EIDRM);
    CHECK(msgctl(id, IPC_STAT, &ds) == -1 && errno == EINVAL);
    CHECK(snd(id, 1, "x", IPC_NOWAIT) == -1 && errno == EINVAL);
    int id2 = msgget(key, IPC_CREAT | 0600); /* may reuse the slot, never the identifier */
    CHECK(id2 >= 0 && id2 != id);
    CHECK(msgctl(id2, IPC_RMID, NULL) == 0);
    CHECK(msgctl(id_priv, IPC_RMID, NULL) == 0);

    puts("TEST OK");
    return 0;
}
