/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/*
 * eintr.c -- does a signal end a blocking call? One case per line: a thread (or a child process)
 * blocks, another thread (or the parent) sends a signal after 100 ms, and the case reports the
 * call's result and how long after the signal it returned. Without an interruptible PAL wait the
 * blocked call never returns, so every case has its own deadline (alarm-free: a watchdog thread
 * prints "TIMEOUT" and exits the process with status 2 after 5 s).
 *
 * Each case also keeps a value in d8 (AArch64; xmm6 is not callee-saved on x86_64, so there the
 * check is skipped) across the blocking call and reports whether it survived: the AAPCS makes
 * d8-d15 callee-saved, and a system call (and a signal handler run during it) must not change them.
 *
 * Cases: poll() on a pipe, ppoll(NULL, 0, NULL) (= pause() on AArch64), pause(), read() of an
 * empty pipe, epoll_wait(), select(), nanosleep(), futex wait (pthread_cond_wait), and a child in
 * pause() killed with SIGTERM by its parent (LTP waitpid03).
 */
#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/select.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t g_handled;
static pid_t g_tid;
static struct timespec g_sent;

static double ms_since(const struct timespec* t) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (now.tv_sec - t->tv_sec) * 1e3 + (now.tv_nsec - t->tv_nsec) / 1e6;
}

static struct timespec g_handled_at;

static void handler(int sig) {
    (void)sig;
    clock_gettime(CLOCK_MONOTONIC, &g_handled_at);
    g_handled++;
}

static void* sender(void* arg) {
    (void)arg;
    usleep(100 * 1000);
    clock_gettime(CLOCK_MONOTONIC, &g_sent);
    syscall(SYS_tgkill, getpid(), g_tid, SIGUSR1);
    return NULL;
}

static void* watchdog(void* arg) {
    sleep(5);
    printf("%s: TIMEOUT (the blocked call did not return 5 s after the signal)\n", (char*)arg);
    fflush(stdout);
    _exit(2);
}

static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cond = PTHREAD_COND_INITIALIZER;

static int run_case(const char* name) {
    int fds[2];
    if (pipe(fds) < 0)
        return -1;
    g_handled = 0;
    g_tid = gettid();
    pthread_t s, w;
    pthread_create(&w, NULL, watchdog, (void*)name);
    pthread_create(&s, NULL, sender, NULL);

    long ret = 0;
#ifdef __aarch64__
    register double keep __asm__("d8") = 1234.5 + (double)strlen(name);
    __asm__ volatile("" : "+w"(keep));
#endif
    errno = 0;
    if (!strcmp(name, "poll")) {
        struct pollfd p = {.fd = fds[0], .events = POLLIN};
        ret = poll(&p, 1, -1);
    } else if (!strcmp(name, "ppoll-null")) {
        ret = ppoll(NULL, 0, NULL, NULL);
    } else if (!strcmp(name, "pause")) {
        ret = pause();
    } else if (!strcmp(name, "pipe-read")) {
        char c;
        ret = read(fds[0], &c, 1);
    } else if (!strcmp(name, "epoll_wait")) {
        int ep = epoll_create1(0);
        struct epoll_event ev = {.events = EPOLLIN, .data.fd = fds[0]};
        epoll_ctl(ep, EPOLL_CTL_ADD, fds[0], &ev);
        ret = epoll_wait(ep, &ev, 1, -1);
        close(ep);
    } else if (!strcmp(name, "select")) {
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(fds[0], &rd);
        ret = select(fds[0] + 1, &rd, NULL, NULL, NULL);
    } else if (!strcmp(name, "nanosleep")) {
        struct timespec t = {.tv_sec = 10};
        ret = nanosleep(&t, NULL);
    } else if (!strcmp(name, "futex")) {
        pthread_mutex_lock(&g_mutex);
        /* a cond wait restarts after the handler (futex: SA_RESTART-like); the handler's run is
         * what shows here, the wait itself is ended by the timed variant */
        struct timespec t;
        clock_gettime(CLOCK_REALTIME, &t);
        t.tv_sec += 1;
        ret = pthread_cond_timedwait(&g_cond, &g_mutex, &t);
        pthread_mutex_unlock(&g_mutex);
        errno = (int)ret;
    }
    int err = errno;
    const char* fp = "n/a";
#ifdef __aarch64__
    __asm__ volatile("" : "+w"(keep));
    fp = keep == 1234.5 + (double)strlen(name) ? "kept" : "CHANGED";
#endif
    double ms = ms_since(&g_sent);
    double handler_ms = g_handled ? ms - ms_since(&g_handled_at) : -1;
    pthread_join(s, NULL);
    pthread_cancel(w);
    pthread_join(w, NULL);
    printf("%-11s ret=%ld errno=%s handler=%d (at %.1f ms) returned %.1f ms after the signal, "
           "d8 %s\n", name, ret, err ? strerror(err) : "0", (int)g_handled, handler_ms, ms, fp);
    fflush(stdout);
    close(fds[0]);
    close(fds[1]);
    return 0;
}

/* LTP waitpid03: a child in pause() killed by the parent's SIGTERM */
static int child_case(void) {
    pid_t pid = fork();
    if (pid < 0) {
        printf("child-pause: fork failed: %s\n", strerror(errno));
        return -1;
    }
    if (pid == 0) {
        pause();
        _exit(0);
    }
    usleep(200 * 1000);
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    kill(pid, SIGTERM);
    int status = 0;
    for (int i = 0; i < 500; i++) {
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) {
            printf("child-pause killed=%d sig=%d after %.1f ms\n", WIFSIGNALED(status),
                   WIFSIGNALED(status) ? WTERMSIG(status) : 0, ms_since(&t));
            return 0;
        }
        usleep(10 * 1000);
    }
    printf("child-pause: TIMEOUT (the child in pause() did not die 5 s after SIGTERM)\n");
    kill(pid, SIGKILL);
    return -1;
}

int main(int argc, char** argv) {
    struct sigaction sa = {.sa_handler = handler};
    sigaction(SIGUSR1, &sa, NULL);
    setvbuf(stdout, NULL, _IOLBF, 0);
    const char* all[] = {"poll", "ppoll-null", "pause", "pipe-read", "epoll_wait", "select",
                         "nanosleep", "futex", "child-pause", NULL};
    for (int i = 0; all[i]; i++) {
        if (argc > 1) {
            int wanted = 0;
            for (int j = 1; j < argc; j++)
                wanted |= !strcmp(argv[j], all[i]);
            if (!wanted)
                continue;
        }
        if (!strcmp(all[i], "child-pause"))
            child_case();
        else
            run_case(all[i]);
    }
    return 0;
}
