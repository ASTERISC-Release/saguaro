/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/*
 * System V IPC between a parent and its forked children (gramine-vm with
 * sys.experimental__multiprocess, or natively): shared memory, message queues and semaphores by key
 * and by IPC_PRIVATE identifier, both directions, IPC_RMID while attached and while another process
 * waits, SEM_UNDO at exit, and no segment file left behind. Prints one "PASS"/"FAIL" line per check
 * and "sysv-fork: N/N passed" at the end; exits 0 when all passed.
 */

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/sem.h>
#include <sys/shm.h>
#include <sys/wait.h>
#include <unistd.h>

static int g_checks, g_passed;

static void check(const char* what, int ok) {
    g_checks++;
    g_passed += !!ok;
    printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    fflush(stdout);
}

struct msg {
    long type;
    char text[64];
};

static int child_status(pid_t pid) {
    int st = 0;
    if (waitpid(pid, &st, 0) != pid || !WIFEXITED(st))
        return -1;
    return WEXITSTATUS(st);
}

static int count_segment_files(void) {
    int n = 0;
    const char* dirs[] = {"/dev/shm", "/tmp"};
    for (size_t i = 0; i < 2; i++) {
        DIR* d = opendir(dirs[i]);
        if (!d)
            continue;
        struct dirent* e;
        while ((e = readdir(d)))
            n += !strncmp(e->d_name, ".gramine-sysv-shm-", 18);
        closedir(d);
    }
    return n;
}

int main(void) {
    key_t shm_key = 0x51510001, msg_key = 0x51510002, sem_key = 0x51510003;

    /* shared memory by key and by inherited attachment */
    int shmid = shmget(shm_key, 8192, IPC_CREAT | IPC_EXCL | 0600);
    char* a = shmat(shmid, NULL, 0);
    check("shmget/shmat in the parent", shmid >= 0 && a != (void*)-1);
    strcpy(a, "parent");
    int priv = shmget(IPC_PRIVATE, 4096, 0600);
    char* p = shmat(priv, NULL, 0);
    strcpy(p, "private");

    int msqid = msgget(msg_key, IPC_CREAT | 0600);
    int semid = semget(sem_key, 2, IPC_CREAT | 0600);
    check("msgget/semget in the parent", msqid >= 0 && semid >= 0);
    semctl(semid, 0, SETVAL, 0);
    semctl(semid, 1, SETVAL, 5);

    pid_t c = fork();
    if (c == 0) {
        int ok = 1, step = 0;
#define STEP(cond) do { step++; if (!(cond)) { ok = 0; printf("child step %d failed (errno %d)\n", step, errno); fflush(stdout); } } while (0)
        STEP(!strcmp(a, "parent"));                          /* inherited attachment */
        STEP(shmget(shm_key, 0, 0600) == shmid);             /* the same segment by key */
        char* b = shmat(shmid, NULL, 0);                     /* a second attach by id */
        STEP(b != (void*)-1 && !strcmp(b, "parent"));
        strcpy(b + 4096, "child");
        STEP(shmdt(b) == 0 && shmdt(a) == 0);                /* the inherited one detaches too */
        char* q = shmat(priv, NULL, 0);                      /* IPC_PRIVATE by identifier */
        STEP(q != (void*)-1 && !strcmp(q, "private"));
        STEP(msgget(msg_key, 0600) == msqid && semget(sem_key, 0, 0600) == semid);
        struct msg m = {7, "from the child"};
        STEP(msgsnd(msqid, &m, sizeof(m.text), 0) == 0);
        struct msg r = {0};
        STEP(msgrcv(msqid, &r, sizeof(r.text), 8, 0) == sizeof(r.text)); /* waits for the parent */
        STEP(!strcmp(r.text, "from the parent"));
        struct sembuf up = {0, 1, 0};
        STEP(semop(semid, &up, 1) == 0);                     /* wakes the parent */
        struct sembuf take = {1, -2, SEM_UNDO};
        STEP(semop(semid, &take, 1) == 0);                   /* undone at exit: back to 5 */
        _exit(ok ? 0 : 1);
    }
    struct msg r = {0};
    int got = msgrcv(msqid, &r, sizeof(r.text), 7, 0);
    check("parent receives the child's message", got == sizeof(r.text) &&
          !strcmp(r.text, "from the child"));
    struct msg m = {8, "from the parent"};
    check("parent sends to the child", msgsnd(msqid, &m, sizeof(m.text), 0) == 0);
    struct sembuf down = {0, -1, 0};
    check("parent's semop is woken by the child", semop(semid, &down, 1) == 0);
    check("child's checks (shm by key/id/inheritance, msg both ways, semop)", child_status(c) == 0);
    check("child's store to the segment", !strcmp(a + 4096, "child"));
    check("SEM_UNDO applied at the child's exit", semctl(semid, 1, GETVAL) == 5);
    struct shmid_ds ds;
    check("nattach after the child detached and exited", shmctl(shmid, IPC_STAT, &ds) == 0 &&
          ds.shm_nattch == 1);

    /* IPC_RMID while a child waits: it wakes with EIDRM */
    c = fork();
    if (c == 0) {
        struct msg x;
        int ret = msgrcv(msqid, &x, sizeof(x.text), 99, 0);
        struct sembuf wait0 = {1, -100, 0};
        int ret2 = semop(semid, &wait0, 1);
        _exit(ret == -1 && errno == EIDRM && ret2 == -1 && errno == EIDRM ? 0 : 1);
    }
    usleep(300000);
    check("msgctl(IPC_RMID)", msgctl(msqid, IPC_RMID, NULL) == 0);
    usleep(300000);
    check("semctl(IPC_RMID)", semctl(semid, 0, IPC_RMID) == 0);
    check("waiters in the child see EIDRM", child_status(c) == 0);
    check("removed key is gone", msgget(msg_key, 0600) == -1 && errno == ENOENT);

    /* a segment removed while attached stays until the last detach, then its file goes */
    int files_before = count_segment_files();
    check("shmctl(IPC_RMID) while attached", shmctl(shmid, IPC_RMID, NULL) == 0);
    check("removed segment still readable", !strcmp(a, "parent"));
    check("removed key no longer found", shmget(shm_key, 0, 0600) == -1 && errno == ENOENT);
    shmdt(a);
    shmctl(priv, IPC_RMID, NULL);
    shmdt(p);
    int files_after = count_segment_files();
    check("segment files removed after the last detach", files_after == files_before - 2 ||
          files_after == 0);

    /* load: 4 children each send 500 messages and post a semaphore per message; the parent
     * receives all 2,000 and takes the semaphore 2,000 times */
    int lq = msgget(IPC_PRIVATE, 0600), ls = semget(IPC_PRIVATE, 1, 0600);
    for (int k = 0; k < 4; k++) {
        if (fork() == 0) {
            struct msg lm = {1 + k, ""};
            struct sembuf up1 = {0, 1, 0};
            for (int i = 0; i < 500; i++) {
                snprintf(lm.text, sizeof(lm.text), "%d:%d", k, i);
                if (msgsnd(lq, &lm, sizeof(lm.text), 0) < 0 || semop(ls, &up1, 1) < 0)
                    _exit(1);
            }
            _exit(0);
        }
    }
    int received = 0, last[4] = {-1, -1, -1, -1}, in_order = 1;
    struct sembuf down1 = {0, -1, 0};
    for (int i = 0; i < 2000; i++) {
        struct msg lr;
        if (semop(ls, &down1, 1) < 0 || msgrcv(lq, &lr, sizeof(lr.text), 0, 0) < 0)
            break;
        int k = 0, n = 0;
        sscanf(lr.text, "%d:%d", &k, &n);
        in_order &= k >= 0 && k < 4 && n == last[k] + 1;
        if (k >= 0 && k < 4)
            last[k] = n;
        received++;
    }
    int bad = 0;
    for (int k = 0; k < 4; k++)
        bad += wait(NULL) < 0;
    check("2,000 messages and semaphore posts from 4 children, in order per sender",
          received == 2000 && in_order && !bad);
    msgctl(lq, IPC_RMID, NULL);
    semctl(ls, 0, IPC_RMID);

    printf("sysv-fork: %d/%d passed\n", g_passed, g_checks);
    return g_passed == g_checks ? 0 : 1;
}
