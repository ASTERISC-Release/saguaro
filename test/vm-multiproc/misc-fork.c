/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/*
 * The dev-libos-misc system calls seen from a forked child (integration probe for the VM PAL's
 * experimental multiprocess support; also runs natively and under gramine-direct for comparison).
 * Prints one line per check, "name: ok" or "name: <what happened>", and "misc-fork done".
 * Usage: misc-fork <scratch-dir>
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/ipc.h>
#include <sys/mman.h>
#include <sys/msg.h>
#include <sys/shm.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/timerfd.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static const char* g_dir;

static void path(char* buf, size_t size, const char* name) {
    snprintf(buf, size, "%s/%s", g_dir, name);
}

/* runs `fn` in a child; returns its exit code (0 = ok), or 100 + signal */
static int in_child(int (*fn)(void)) {
    fflush(stdout);
    pid_t pid = fork();
    if (pid < 0) {
        printf("fork failed: %s\n", strerror(errno));
        return -1;
    }
    if (pid == 0)
        _exit(fn());
    int st;
    if (waitpid(pid, &st, 0) != pid)
        return -2;
    return WIFEXITED(st) ? WEXITSTATUS(st) : 100 + WTERMSIG(st);
}

static void report(const char* name, int rc, const char* what) {
    if (rc == 0)
        printf("%s: ok\n", name);
    else
        printf("%s: rc=%d %s\n", name, rc, what ? what : "");
}

/* --- SysV shared memory: an attach inherited over fork, and the child's segment table --- */
static volatile int* g_shm;
static int g_shmid;

static int child_shm(void) {
    if (g_shm[0] != 1234)
        return 1; /* parent's store not visible */
    g_shm[1] = 5678;
    /* the table is process-local (dev-libos-misc): the child does not know the id */
    struct shmid_ds ds;
    int r = shmctl(g_shmid, IPC_STAT, &ds);
    printf("  shm child: IPC_STAT of the parent's id -> %s\n", r == 0 ? "0" : strerror(errno));
    return 0;
}

static void check_shm(void) {
    g_shmid = shmget(IPC_PRIVATE, 4096, IPC_CREAT | 0600);
    if (g_shmid < 0) {
        report("sysv_shm", -1, strerror(errno));
        return;
    }
    g_shm = shmat(g_shmid, NULL, 0);
    if (g_shm == (void*)-1) {
        report("sysv_shm", -1, strerror(errno));
        return;
    }
    g_shm[0] = 1234;
    int rc = in_child(child_shm);
    if (rc == 0 && g_shm[1] != 5678) {
        rc = 2;
        report("sysv_shm (child store visible to parent)", rc, "no");
    } else {
        report("sysv_shm (inherited attach shared both ways)", rc, rc == 1 ? "parent store not seen" : "");
    }
    shmdt((void*)g_shm);
    shmctl(g_shmid, IPC_RMID, NULL);
}

/* --- SysV message queues: a queue of the parent, from the child --- */
static int g_msqid;
struct msgbuf_s { long mtype; char text[16]; };

static int child_msg(void) {
    struct msgbuf_s m;
    ssize_t n = msgrcv(g_msqid, &m, sizeof(m.text), 0, IPC_NOWAIT);
    printf("  msg child: msgrcv on the parent's queue -> %s\n", n >= 0 ? m.text : strerror(errno));
    /* a queue of its own works */
    int q = msgget(IPC_PRIVATE, IPC_CREAT | 0600);
    if (q < 0)
        return 1;
    struct msgbuf_s s = { .mtype = 1 };
    strcpy(s.text, "child");
    if (msgsnd(q, &s, sizeof(s.text), 0) < 0)
        return 2;
    if (msgrcv(q, &m, sizeof(m.text), 1, IPC_NOWAIT) < 0 || strcmp(m.text, "child"))
        return 3;
    msgctl(q, IPC_RMID, NULL);
    return 0;
}

static void check_msg(void) {
    g_msqid = msgget(IPC_PRIVATE, IPC_CREAT | 0600);
    if (g_msqid < 0) {
        report("sysv_msg", -1, strerror(errno));
        return;
    }
    struct msgbuf_s s = { .mtype = 1 };
    strcpy(s.text, "parent");
    msgsnd(g_msqid, &s, sizeof(s.text), 0);
    report("sysv_msg (child's own queue)", in_child(child_msg), "");
    msgctl(g_msqid, IPC_RMID, NULL);
}

/* --- memfd: inherited fd and MAP_SHARED mapping --- */
static int g_memfd;
static volatile char* g_memmap;

static int child_memfd(void) {
    char buf[8] = {0};
    if (pread(g_memfd, buf, 3, 0) != 3 || memcmp(buf, "abc", 3))
        return 1;
    if (g_memmap[0] != 'a')
        return 2;
    g_memmap[1] = 'X';
    return 0;
}

static void check_memfd(void) {
    g_memfd = syscall(SYS_memfd_create, "probe", 0);
    if (g_memfd < 0) {
        report("memfd (memfd_create)", -1, strerror(errno));
        return;
    }
    if (write(g_memfd, "abc", 3) != 3) {
        report("memfd (write)", -1, strerror(errno));
        return;
    }
    if (ftruncate(g_memfd, 4096) < 0) {
        report("memfd (ftruncate)", -1, strerror(errno));
        return;
    }
    g_memmap = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, g_memfd, 0);
    if (g_memmap == MAP_FAILED) {
        report("memfd (mmap MAP_SHARED)", -1, strerror(errno));
        return;
    }
    int rc = in_child(child_memfd);
    char c = 0;
    if (pread(g_memfd, &c, 1, 1) != 1)
        c = 0;
    printf("  memfd parent: map[1]=%c, read()[1]=%c after the child's store\n", g_memmap[1], c);
    report("memfd (child reads the parent's data)", rc, rc == 1 ? "read" : rc == 2 ? "map" : "");
    munmap((void*)g_memmap, 4096);
    close(g_memfd);
}

/* --- file calls of the new PAL interface, made by a child --- */
static int child_files(void) {
    char a[512], b[512], c[512], d[512];
    path(a, sizeof(a), "mf-a");
    path(b, sizeof(b), "mf-b");
    path(c, sizeof(c), "mf-sym");
    path(d, sizeof(d), "mf-c");
    int fd = open(a, O_CREAT | O_WRONLY | O_TRUNC, 0600);
    if (fd < 0 || write(fd, "x", 1) != 1)
        return 1;
    close(fd);
    if (link(a, b) < 0)
        return 2;
    if (symlink("mf-a", c) < 0)
        return 3;
    char t[64] = {0};
    if (readlink(c, t, sizeof(t) - 1) != 4 || strcmp(t, "mf-a"))
        return 4;
    struct timespec ts[2] = { { .tv_sec = 1000000000 }, { .tv_sec = 1000000001 } };
    if (utimensat(AT_FDCWD, a, ts, 0) < 0)
        return 5;
    struct stat st;
    if (stat(b, &st) < 0 || st.st_mtime != 1000000001 || st.st_nlink != 2)
        return 6;
    if (syscall(SYS_renameat2, AT_FDCWD, b, AT_FDCWD, d, 1 /* RENAME_NOREPLACE */) < 0)
        return 7;
    if (unlink(a) < 0 || unlink(c) < 0 || unlink(d) < 0)
        return 8;
    return 0;
}

/* --- timerfd and POSIX timer created by a child --- */
static int child_timers(void) {
    int tfd = timerfd_create(CLOCK_MONOTONIC, 0);
    if (tfd < 0)
        return 1;
    struct itimerspec its = { .it_value = { .tv_nsec = 20 * 1000 * 1000 } };
    if (timerfd_settime(tfd, 0, &its, NULL) < 0)
        return 2;
    uint64_t exp;
    if (read(tfd, &exp, sizeof(exp)) != sizeof(exp) || exp != 1)
        return 3;
    close(tfd);

    timer_t tid;
    struct sigevent sev = { .sigev_notify = SIGEV_SIGNAL, .sigev_signo = SIGUSR1 };
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    sigprocmask(SIG_BLOCK, &set, NULL);
    if (timer_create(CLOCK_MONOTONIC, &sev, &tid) < 0)
        return 4;
    if (timer_settime(tid, 0, &its, NULL) < 0)
        return 5;
    int sig;
    if (sigwait(&set, &sig) != 0 || sig != SIGUSR1)
        return 6;
    timer_delete(tid);
    return 0;
}

/* --- inotify used by a child --- */
static int child_inotify(void) {
    int in = inotify_init1(0);
    if (in < 0)
        return 1;
    if (inotify_add_watch(in, g_dir, IN_CREATE) < 0)
        return 2;
    char p[512];
    path(p, sizeof(p), "mf-ino");
    int fd = open(p, O_CREAT | O_WRONLY, 0600);
    if (fd < 0)
        return 3;
    close(fd);
    char buf[4096];
    ssize_t n = read(in, buf, sizeof(buf));
    unlink(p);
    if (n < (ssize_t)sizeof(struct inotify_event))
        return 4;
    struct inotify_event* ev = (struct inotify_event*)buf;
    return (ev->mask & IN_CREATE) && !strcmp(ev->name, "mf-ino") ? 0 : 5;
}

int main(int argc, char** argv) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    g_dir = argc > 1 ? argv[1] : "/tmp";
    check_shm();
    check_msg();
    check_memfd();
    report("files in a child (link, symlink, utimensat, renameat2, unlink)", in_child(child_files), "");
    report("timers in a child (timerfd, POSIX timer)", in_child(child_timers), "");
    report("inotify in a child", in_child(child_inotify), "");
    printf("misc-fork done\n");
    return 0;
}
