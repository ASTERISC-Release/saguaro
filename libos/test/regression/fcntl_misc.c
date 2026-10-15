/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/*
 * fcntl() commands beyond locks and flags (LTP fcntl23-26, fcntl32, fcntl37, fcntl38):
 * F_SETLEASE/F_GETLEASE with the kernel's conflict rules, F_SETSIG/F_GETSIG, F_SETOWN/F_GETOWN,
 * F_GETPIPE_SZ/F_SETPIPE_SZ with /proc/sys/fs/pipe-max-size, and dnotify (F_NOTIFY) delivering
 * the F_SETSIG signal with si_fd for a change in a directory and of the directory itself.
 *
 * Usage: fcntl_misc <scratch directory>
 */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHECK(cond)                                                \
    do {                                                           \
        if (!(cond)) {                                             \
            fprintf(stderr, "%s:%d: %s failed (errno=%d)\n",       \
                    __FILE__, __LINE__, #cond, errno);             \
            exit(1);                                               \
        }                                                          \
    } while (0)

static volatile int g_parent_events, g_child_events, g_parent_fd, g_child_fd;

static void on_dnotify(int sig, siginfo_t* si, void* ctx) {
    (void)sig;
    (void)ctx;
    if (si->si_fd == g_parent_fd)
        g_parent_events++;
    else if (si->si_fd == g_child_fd)
        g_child_events++;
}

int main(int argc, char** argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: fcntl_misc <dir>\n");
        return 1;
    }
    char file[512], sub[512];
    snprintf(file, sizeof(file), "%s/fcntl_misc_file", argv[1]);
    snprintf(sub, sizeof(sub), "%s/fcntl_misc_dir", argv[1]);
    unlink(file);
    rmdir(sub);

    /* ---- leases ---- */
    int wfd = open(file, O_RDWR | O_CREAT | O_TRUNC, 0644);
    CHECK(wfd >= 0);
    CHECK(fcntl(wfd, F_GETLEASE) == F_UNLCK);
    CHECK(fcntl(wfd, F_SETLEASE, F_WRLCK) == 0); /* the only open description */
    CHECK(fcntl(wfd, F_GETLEASE) == F_WRLCK);
    CHECK(fcntl(wfd, F_SETLEASE, F_RDLCK) == -1 && errno == EAGAIN); /* it is a writer */
    CHECK(fcntl(wfd, F_SETLEASE, F_UNLCK) == 0);
    CHECK(fcntl(wfd, F_GETLEASE) == F_UNLCK);
    CHECK(fcntl(wfd, F_SETLEASE, 12345) == -1 && errno == EINVAL);

    int rfd = open(file, O_RDONLY);
    CHECK(rfd >= 0);
    CHECK(fcntl(rfd, F_SETLEASE, F_RDLCK) == -1 && errno == EAGAIN); /* another writer is open */
    CHECK(fcntl(wfd, F_SETLEASE, F_WRLCK) == -1 && errno == EAGAIN); /* a reader is open */
    close(wfd);
    CHECK(fcntl(rfd, F_SETLEASE, F_RDLCK) == 0);
    CHECK(fcntl(rfd, F_GETLEASE) == F_RDLCK);
    int dupfd = dup(rfd); /* a dup shares the open description: still the same lease */
    CHECK(fcntl(dupfd, F_GETLEASE) == F_RDLCK);
    CHECK(fcntl(rfd, F_SETLEASE, F_UNLCK) == 0);
    close(dupfd);
    close(rfd);

    int pfd[2];
    CHECK(pipe(pfd) == 0);
    CHECK(fcntl(pfd[0], F_SETLEASE, F_RDLCK) == -1 && errno == EINVAL); /* not a regular file */

    /* ---- F_SETSIG / F_SETOWN ---- */
    CHECK(fcntl(pfd[0], F_GETSIG) == 0);
    CHECK(fcntl(pfd[0], F_SETSIG, SIGRTMIN) == 0 && fcntl(pfd[0], F_GETSIG) == SIGRTMIN);
    CHECK(fcntl(pfd[0], F_SETSIG, 1000) == -1 && errno == EINVAL);
    CHECK(fcntl(pfd[0], F_SETOWN, getpid()) == 0 && fcntl(pfd[0], F_GETOWN) == getpid());

    /* ---- pipe capacity ---- */
    int def = fcntl(pfd[1], F_GETPIPE_SZ); /* 65536, less when the user's pipe budget is low */
    CHECK(def >= 4096 && def % 4096 == 0);
    /* rounded up to a power-of-two number of pages (kept small: growing a pipe can run into the
     * user's pipe budget natively) */
    CHECK(fcntl(pfd[1], F_SETPIPE_SZ, 5000) == 8192);
    CHECK(fcntl(pfd[0], F_GETPIPE_SZ) == 8192); /* one pipe, both ends */
    CHECK(fcntl(pfd[1], F_SETPIPE_SZ, (1U << 31) + 1) == -1 && errno == EINVAL);
    char data[8192];
    memset(data, 'x', sizeof(data));
    /* non-blocking: the VM PAL's pipe buffers only one page (see the libos-syscalls TODO.md) */
    CHECK(fcntl(pfd[1], F_SETFL, O_NONBLOCK) == 0);
    if (write(pfd[1], data, sizeof(data)) == (ssize_t)sizeof(data))
        CHECK(fcntl(pfd[1], F_SETPIPE_SZ, 4096) == -1 && errno == EBUSY); /* holds more */
    FILE* f = fopen("/proc/sys/fs/pipe-max-size", "r");
    CHECK(f);
    int max = 0;
    CHECK(fscanf(f, "%d", &max) == 1 && max == 1048576);
    fclose(f);
    close(pfd[0]);
    close(pfd[1]);

    /* ---- dnotify ---- */
    CHECK(mkdir(sub, 0700) == 0);
    struct sigaction sa = {.sa_sigaction = on_dnotify, .sa_flags = SA_SIGINFO};
    sigemptyset(&sa.sa_mask);
    CHECK(sigaction(SIGRTMIN + 1, &sa, NULL) == 0);
    g_parent_fd = open(argv[1], O_RDONLY | O_DIRECTORY);
    g_child_fd = open(sub, O_RDONLY | O_DIRECTORY);
    CHECK(g_parent_fd >= 0 && g_child_fd >= 0);
    int ffd = open(file, O_RDONLY);
    CHECK(ffd >= 0);
    CHECK(fcntl(ffd, F_NOTIFY, DN_ATTRIB) == -1 && errno == ENOTDIR);
    close(ffd);
    for (int i = 0; i < 2; i++) {
        int fd = i ? g_child_fd : g_parent_fd;
        CHECK(fcntl(fd, F_SETSIG, SIGRTMIN + 1) == 0);
        CHECK(fcntl(fd, F_NOTIFY, DN_ATTRIB | DN_MULTISHOT) == 0);
    }
    CHECK(chmod(sub, 0755) == 0); /* an entry of the parent changes, and the directory itself */
    CHECK(g_parent_events >= 1 && g_child_events >= 1);
    close(g_parent_fd);
    close(g_child_fd);
    /* a one-shot mark (no DN_MULTISHOT) fires once */
    g_parent_fd = -1;
    g_child_fd = open(sub, O_RDONLY | O_DIRECTORY);
    CHECK(g_child_fd >= 0);
    CHECK(fcntl(g_child_fd, F_SETSIG, SIGRTMIN + 1) == 0);
    CHECK(fcntl(g_child_fd, F_NOTIFY, DN_ATTRIB) == 0);
    int before = g_child_events;
    CHECK(chmod(sub, 0700) == 0);
    CHECK(chmod(sub, 0755) == 0);
    CHECK(g_child_events == before + 1);
    close(g_child_fd);
    CHECK(chmod(sub, 0700) == 0); /* closed descriptors: no more signals, nothing crashes */

    rmdir(sub);
    unlink(file);
    puts("TEST OK");
    return 0;
}
