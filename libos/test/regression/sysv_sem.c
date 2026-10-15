/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/*
 * System V semaphores in one process (threads instead of fork, seteuid for permissions): sets of
 * several semaphores, every semctl() command, atomic multi-operation semop(), IPC_NOWAIT,
 * semtimedop() timeouts, GETNCNT/GETZCNT of a blocked thread, wake-up by another thread, EIDRM
 * after IPC_RMID, and /proc/sys/kernel/sem (LTP semget/semctl/semop).
 */

#define _GNU_SOURCE

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/sem.h>
#include <time.h>
#include <unistd.h>

#define CHECK(cond)                                                \
    do {                                                           \
        if (!(cond)) {                                             \
            fprintf(stderr, "%s:%d: %s failed (errno=%d)\n",       \
                    __FILE__, __LINE__, #cond, errno);             \
            exit(1);                                               \
        }                                                          \
    } while (0)

union semun {
    int val;
    struct semid_ds* buf;
    unsigned short* array;
    struct seminfo* __buf;
};

static int g_id;
static volatile int g_ret = -2, g_errno;

static void* waiter(void* arg) {
    struct sembuf op = {.sem_num = 1, .sem_op = (short)(long)arg, .sem_flg = 0};
    errno = 0;
    g_ret = semop(g_id, &op, 1);
    g_errno = errno;
    return NULL;
}

static void sleep_ms(long ms) {
    struct timespec ts = {.tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000L};
    while (nanosleep(&ts, &ts) < 0 && errno == EINTR)
        ;
}

static void start(pthread_t* t, long op) {
    g_ret = -2;
    CHECK(pthread_create(t, NULL, waiter, (void*)op) == 0);
    sleep_ms(200);
    CHECK(g_ret == -2);
}

int main(void) {
    const key_t key = 0x5e1f0002;
    CHECK(semget(key, -1, IPC_CREAT | 0600) == -1 && errno == EINVAL);
    CHECK(semget(key, 3, 0600) == -1 && errno == ENOENT);
    CHECK(semget(key, 0, IPC_CREAT | 0600) == -1 && errno == EINVAL);
    int id = semget(key, 3, IPC_CREAT | IPC_EXCL | 0600);
    CHECK(id >= 0);
    g_id = id;
    CHECK(semget(key, 3, IPC_CREAT | IPC_EXCL | 0600) == -1 && errno == EEXIST);
    CHECK(semget(key, 4, 0600) == -1 && errno == EINVAL); /* more than the set has */
    CHECK(semget(key, 0, 0) == id);

    /* values */
    union semun u;
    struct semid_ds ds;
    u.buf = &ds;
    CHECK(semctl(id, 0, IPC_STAT, u) == 0 && ds.sem_nsems == 3 && (ds.sem_perm.mode & 0777) == 0600);
    u.val = 5;
    CHECK(semctl(id, 0, SETVAL, u) == 0);
    CHECK(semctl(id, 0, GETVAL) == 5 && semctl(id, 0, GETPID) == getpid());
    u.val = 40000;
    CHECK(semctl(id, 0, SETVAL, u) == -1 && errno == ERANGE);
    CHECK(semctl(id, 3, GETVAL) == -1 && errno == EINVAL);
    unsigned short vals[3] = {1, 2, 3};
    u.array = vals;
    CHECK(semctl(id, 0, SETALL, u) == 0);
    unsigned short out[3] = {0};
    u.array = out;
    CHECK(semctl(id, 0, GETALL, u) == 0 && out[0] == 1 && out[1] == 2 && out[2] == 3);

    /* atomic operations: all or nothing */
    struct sembuf ops[2] = {{0, -1, IPC_NOWAIT}, {2, -5, IPC_NOWAIT}};
    CHECK(semop(id, ops, 2) == -1 && errno == EAGAIN);
    CHECK(semctl(id, 0, GETVAL) == 1); /* the first operation was not kept */
    ops[1].sem_op = -3;
    CHECK(semop(id, ops, 2) == 0);
    CHECK(semctl(id, 0, GETVAL) == 0 && semctl(id, 2, GETVAL) == 0);
    struct sembuf big = {7, 1, 0};
    CHECK(semop(id, &big, 1) == -1 && errno == EFBIG);
    CHECK(semop(id, ops, 0) == -1 && errno == EINVAL);
    struct sembuf many[501];
    memset(many, 0, sizeof(many));
    CHECK(semop(id, many, 501) == -1 && errno == E2BIG);
    struct sembuf over = {1, 32767, 0};
    CHECK(semop(id, &over, 1) == -1 && errno == ERANGE);

    /* semtimedop: the timeout expires with EAGAIN */
    struct sembuf dec = {0, -1, 0};
    struct timespec ts = {0, 100 * 1000 * 1000};
    CHECK(semtimedop(id, &dec, 1, &ts) == -1 && errno == EAGAIN);

    /* a blocked decrement: GETNCNT, then woken by another thread's increment */
    pthread_t t;
    start(&t, -3); /* semaphore 1 holds 2 */
    CHECK(semctl(id, 1, GETNCNT) == 1 && semctl(id, 1, GETZCNT) == 0);
    struct sembuf inc = {1, 1, 0};
    CHECK(semop(id, &inc, 1) == 0);
    CHECK(pthread_join(t, NULL) == 0);
    CHECK(g_ret == 0 && semctl(id, 1, GETVAL) == 0);

    /* a wait-for-zero: GETZCNT */
    u.val = 1;
    CHECK(semctl(id, 1, SETVAL, u) == 0);
    start(&t, 0);
    CHECK(semctl(id, 1, GETZCNT) == 1);
    u.val = 0;
    CHECK(semctl(id, 1, SETVAL, u) == 0);
    CHECK(pthread_join(t, NULL) == 0);
    CHECK(g_ret == 0);

    /* queries */
    struct seminfo info;
    u.__buf = &info;
    int max_index = semctl(id, 0, IPC_INFO, u);
    CHECK(max_index >= 0 && info.semmsl == 32000 && info.semopm == 500 && info.semvmx == 32767);
    CHECK(semctl(id, 0, SEM_INFO, u) == max_index);
    u.buf = &ds;
    int found = 0;
    for (int i = 0; i <= max_index; i++)
        if (semctl(i, 0, SEM_STAT, u) == id)
            found = 1;
    CHECK(found);
    FILE* f = fopen("/proc/sys/kernel/sem", "r");
    CHECK(f);
    int msl, mns, opm, mni;
    CHECK(fscanf(f, "%d %d %d %d", &msl, &mns, &opm, &mni) == 4 && msl == 32000 && opm == 500);
    fclose(f);

    /* permissions */
    CHECK(seteuid(65534) == 0);
    CHECK(semctl(id, 0, GETVAL) == -1 && errno == EACCES);
    CHECK(semop(id, &inc, 1) == -1 && errno == EACCES);
    CHECK(semctl(id, 0, IPC_RMID) == -1 && errno == EPERM);
    CHECK(semget(key, 0, 0600) == -1 && errno == EACCES);
    CHECK(seteuid(0) == 0);

    /* IPC_RMID wakes a blocked semop with EIDRM; the identifier then is stale */
    start(&t, -1);
    CHECK(semctl(id, 0, IPC_RMID) == 0);
    CHECK(pthread_join(t, NULL) == 0);
    CHECK(g_ret == -1 && g_errno == EIDRM);
    CHECK(semctl(id, 0, GETVAL) == -1 && errno == EINVAL);

    puts("TEST OK");
    return 0;
}
